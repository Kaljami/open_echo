#pragma once

#include <Arduino.h>

const char WEB_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Open Echo</title>
<style>
:root { color-scheme: dark; }
body { margin: 0; background: #202124; color: #ddd; font: 14px system-ui, sans-serif; }
main { display: flex; flex-direction: column; gap: 8px; padding: 8px; max-width: 1400px; margin: auto; }
.panel { background: #111; border: 1px solid #333; border-radius: 6px; padding: 6px; }
.panel h2 { margin: 0 0 4px; font-size: 13px; font-weight: normal; color: #aaa; text-align: center; }
canvas { display: block; width: 100%; }
#wave { height: 28vh; min-height: 160px; }
#water { height: 42vh; min-height: 220px; }
.readout { display: flex; align-items: center; gap: 16px; flex-wrap: wrap; }
#distance { font-size: 40px; font-weight: 600; color: #ff6b6b; }
fieldset { border: 1px solid #333; border-radius: 6px; display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: center; }
legend { color: #aaa; }
label { display: flex; gap: 6px; align-items: center; }
input[type=number] { width: 80px; background: #2a2a2a; color: #eee; border: 1px solid #444; border-radius: 4px; padding: 3px; }
select, button { background: #2a2a2a; color: #eee; border: 1px solid #444; border-radius: 4px; padding: 4px 8px; }
button:disabled { opacity: 0.5; cursor: not-allowed; }
.muted { color: #999; }
.offline { color: #ff6b6b; }
</style>
</head>
<body>
<main>
  <div class="panel readout">
    <div><div class="muted">First echo</div><div id="distance">--</div></div>
    <div id="echoInfo" class="muted">Waiting for capture.</div>
    <div id="conn" class="muted" style="margin-left:auto">Connecting...</div>
  </div>
  <fieldset>
    <legend>Lucky Finder power</legend>
    <button id="powerOn" disabled>Turn on Lucky Finder</button>
    <span id="powerStatus" class="muted" role="status">Connecting...</span>
  </fieldset>
  <div class="panel"><h2>Latest capture</h2><canvas id="wave"></canvas></div>
  <div class="panel"><h2>Waterfall (each row is one ping, newest at bottom)</h2><canvas id="water"></canvas></div>
  <fieldset>
    <legend>Detection (saved on device)</legend>
    <label title="Samples at the start of each capture (transmit ring-down / blind spot) that are never treated as echoes.">Ignore first samples <input type="number" id="blind" min="0" step="1"></label>
    <label>Distance per sample (cm) <input type="number" id="dps" min="0.001" max="1000" step="0.001"></label>
    <label>Distance offset (cm) <input type="number" id="offset" min="-10000" max="10000" step="0.1"></label>
    <label title="First echo = first peak reaching this percentage of the way from the baseline to the strongest reading. Lower it to catch weaker near echoes.">Echo threshold (%) <input type="number" id="echoPct" min="1" max="100" step="1"></label>
    <label title="Captures whose first samples average below this are treated as non-ping triggers and discarded.">Ping threshold (0 = off) <input type="number" id="pingThr" min="0" max="4095" step="10"></label>
    <span id="saveState" class="muted"></span>
  </fieldset>
  <fieldset>
    <legend>Display (this browser)</legend>
    <label><input type="checkbox" id="autoScale"> Auto-scale waveform</label>
    <label><input type="checkbox" id="markEcho"> Mark echoes in waterfall</label>
    <label>Waterfall min <input type="number" id="levelMin" min="0" max="4094"></label>
    <label>Waterfall max <input type="number" id="levelMax" min="1" max="4095"></label>
    <label>Colors <select id="cmap"></select></label>
    <button id="clear">Clear plot</button>
  </fieldset>
  <div id="stats" class="muted">No captures received yet.</div>
</main>
<script>
const ADC_MAX = 4095, MAX_ROWS = 300, POLL_MS = 250;
const SETTINGS = ['blind', 'dps', 'offset', 'echoPct', 'pingThr'];
const CMAPS = {
  viridis: ['#440154', '#3b528b', '#21918c', '#5ec962', '#fde725'],
  plasma: ['#0d0887', '#7e03a8', '#cc4778', '#f89540', '#f0f921'],
  inferno: ['#000004', '#57106e', '#bc3754', '#f98e09', '#fcffa4'],
  magma: ['#000004', '#51127c', '#b73779', '#fc8961', '#fcfdbf'],
  grey: ['#000000', '#ffffff'],
};
const M = { l: 48, r: 10, t: 6, b: 22 };
const $ = id => document.getElementById(id);

let view = { autoScale: true, markEcho: true, levelMin: 200, levelMax: 2500, cmap: 'viridis' };
try { Object.assign(view, JSON.parse(localStorage.getItem('openEchoView') || '{}')); } catch (e) {}

let rows = [], echoes = [], latest = null, shown = 0, lastSeq = 0;
let data = null, settings = null, lut = null, rejectedBase = null, lateBase = null;
let powerRequestPending = false, powerBusy = false, powerStatusKnown = false;
const offscreen = document.createElement('canvas');

function buildLut(name) {
  const stops = (CMAPS[name] || CMAPS.viridis).map(h => [1, 3, 5].map(i => parseInt(h.substr(i, 2), 16)));
  const out = new Uint8ClampedArray(256 * 3);
  for (let i = 0; i < 256; i++) {
    const t = i / 255 * (stops.length - 1), k = Math.min(Math.floor(t), stops.length - 2), f = t - k;
    for (let c = 0; c < 3; c++) out[i * 3 + c] = stops[k][c] + (stops[k + 1][c] - stops[k][c]) * f;
  }
  return out;
}

function fitCanvas(canvas) {
  const dpr = window.devicePixelRatio || 1, w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}

function xTicks(ctx, n, w, h) {
  const pw = w - M.l - M.r;
  ctx.fillStyle = '#999';
  ctx.font = '11px sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (let s = 0; s < n; s += 50) ctx.fillText(s, M.l + (s + 0.5) / n * pw, h - M.b + 4);
}

function drawWave() {
  const { ctx, w, h } = fitCanvas($('wave'));
  if (!latest) return;
  const n = latest.length, pw = w - M.l - M.r, ph = h - M.t - M.b;
  let lo = 0, hi = ADC_MAX;
  if (view.autoScale) {
    const mn = Math.min(...latest), mx = Math.max(...latest), margin = Math.max(5, (mx - mn) * 0.12);
    lo = Math.max(0, mn - margin);
    hi = Math.min(ADC_MAX, mx + margin);
  }
  const X = s => M.l + (s + 0.5) / n * pw, Y = v => M.t + (1 - (v - lo) / (hi - lo)) * ph;

  if (settings && settings.blind > 0) {
    ctx.fillStyle = 'rgba(255,255,255,0.07)';
    ctx.fillRect(M.l, M.t, settings.blind / n * pw, ph);
  }
  ctx.strokeStyle = '#333';
  ctx.lineWidth = 1;
  ctx.fillStyle = '#999';
  ctx.font = '11px sans-serif';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let i = 0; i <= 4; i++) {
    const v = lo + (hi - lo) * i / 4, y = Y(v);
    ctx.beginPath(); ctx.moveTo(M.l, y); ctx.lineTo(w - M.r, y); ctx.stroke();
    ctx.fillText(Math.round(v), M.l - 4, y);
  }
  xTicks(ctx, n, w, h);

  ctx.strokeStyle = '#eee';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  latest.forEach((v, i) => i ? ctx.lineTo(X(i), Y(v)) : ctx.moveTo(X(i), Y(v)));
  ctx.stroke();

  const e = data && data.echo;
  if (e && e.index < n) {
    ctx.strokeStyle = '#ff4040';
    ctx.beginPath(); ctx.moveTo(X(e.index), M.t); ctx.lineTo(X(e.index), M.t + ph); ctx.stroke();
    ctx.lineWidth = 1;
    ctx.setLineDash([6, 4]);
    ctx.beginPath(); ctx.moveTo(M.l, Y(e.level)); ctx.lineTo(w - M.r, Y(e.level)); ctx.stroke();
    ctx.setLineDash([]);
  }
}

function drawWaterfall() {
  const { ctx, w, h } = fitCanvas($('water'));
  const pw = w - M.l - M.r, ph = h - M.t - M.b;
  ctx.fillStyle = '#000';
  ctx.fillRect(M.l, M.t, pw, ph);
  if (!rows.length) return;
  const n = rows[0].length;
  if (offscreen.width !== n || offscreen.height !== MAX_ROWS) {
    offscreen.width = n;
    offscreen.height = MAX_ROWS;
  }
  const octx = offscreen.getContext('2d'), img = octx.createImageData(n, MAX_ROWS), px = img.data;
  const lo = view.levelMin, span = Math.max(1, view.levelMax - view.levelMin), first = MAX_ROWS - rows.length;
  rows.forEach((row, k) => {
    let p = (first + k) * n * 4;
    for (let s = 0; s < n; s++, p += 4) {
      const c = Math.max(0, Math.min(255, Math.round((row[s] - lo) / span * 255))) * 3;
      px[p] = lut[c]; px[p + 1] = lut[c + 1]; px[p + 2] = lut[c + 2]; px[p + 3] = 255;
    }
    const e = echoes[k];
    if (view.markEcho && e >= 0 && e < n) {
      p = ((first + k) * n + e) * 4;
      px[p] = 255; px[p + 1] = 40; px[p + 2] = 40;
    }
  });
  octx.putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(offscreen, M.l, M.t, pw, ph);
  xTicks(ctx, n, w, h);
}

function updateText() {
  const e = data && latest ? data.echo : null;
  if (e) {
    $('distance').textContent = e.distance < 0 ? 'check calibration' : e.distance.toFixed(1) + ' cm';
    $('echoInfo').textContent = `sample ${e.index} | ${e.counts} counts`;
  } else {
    $('distance').textContent = '--';
    $('echoInfo').textContent = latest ? 'No echo found above the baseline.' : 'Waiting for capture.';
  }
  if (!data) return;
  if (!latest) {
    const discarded = data.rejected - rejectedBase;
    if (discarded > 0) {
      $('stats').textContent = `No pings accepted yet | ${discarded} non-ping captures discarded` +
        ' (if this keeps rising, lower the ping threshold)';
    }
    return;
  }
  let mn = Infinity, mx = -Infinity, sum = 0;
  for (const v of latest) { mn = Math.min(mn, v); mx = Math.max(mx, v); sum += v; }
  $('stats').textContent = `Capture ${shown} | min ${mn} | max ${mx} | mean ${(sum / latest.length).toFixed(1)} | ` +
    `peak-to-peak ${mx - mn} counts | ${data.rejected - rejectedBase} non-ping captures discarded | ` +
    `${data.late - lateBase} late triggers skipped | sample period ${data.samplePeriodUs.toFixed(1)} \u00b5s`;
}

function render() {
  drawWave();
  drawWaterfall();
  updateText();
}

function addCapture(c) {
  if (rows.length && rows[0].length !== c.v.length) { rows = []; echoes = []; }
  rows.push(c.v);
  echoes.push(c.echo);
  if (rows.length > MAX_ROWS) { rows.shift(); echoes.shift(); }
  latest = c.v;
  shown++;
}

function setConnected(ok) {
  $('conn').textContent = ok ? 'Connected' : 'Connection lost, retrying...';
  $('conn').className = ok ? 'muted' : 'offline';
  $('powerOn').disabled = !ok || powerRequestPending || powerBusy;
}

function showPowerState(busy) {
  if (powerRequestPending) return;
  if (powerStatusKnown && busy === powerBusy) return;
  powerStatusKnown = true;
  $('powerStatus').className = 'muted';
  $('powerStatus').textContent = busy ? 'Power-on sequence running (7 seconds)...' :
    powerBusy ? 'Sequence complete; power state is not verified.' : 'Ready (1 s ground, 5 s release, 1 s ground).';
  powerBusy = busy;
}

async function turnOnLuckyFinder() {
  powerRequestPending = true;
  $('powerOn').disabled = true;
  $('powerStatus').className = 'muted';
  $('powerStatus').textContent = 'Starting power-on sequence...';
  try {
    const r = await fetch('/power/on', { method: 'POST' });
    if (!r.ok) throw new Error(await r.text());
    const result = await r.json();
    powerRequestPending = false;
    showPowerState(result.powerBusy);
  } catch (err) {
    powerRequestPending = false;
    $('powerStatus').className = 'offline';
    $('powerStatus').textContent = 'Power-on request failed: ' + err.message + '. Check the device before retrying.';
  }
}

async function poll() {
  try {
    const r = await fetch('/data?since=' + lastSeq, { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    const d = await r.json();
    if (d.seq < lastSeq) { lastSeq = 0; rejectedBase = 0; lateBase = 0; }  // device restarted
    if (rejectedBase === null) { rejectedBase = d.rejected; lateBase = d.late; }
    for (const c of d.captures) {
      if (c.seq > lastSeq) { addCapture(c); lastSeq = c.seq; }
    }
    data = d;
    showPowerState(d.powerBusy);
    render();
    setConnected(true);
  } catch (err) {
    setConnected(false);
  }
  setTimeout(poll, POLL_MS);
}

function showSettings(s) {
  settings = s;
  for (const k of SETTINGS) $(k).value = s[k];
  $('blind').max = s.samples - 1;
}

async function loadSettings() {
  try {
    const r = await fetch('/settings', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    showSettings(await r.json());
  } catch (err) {
    setTimeout(loadSettings, 1000);
  }
}

async function saveSettings() {
  const body = new URLSearchParams();
  for (const k of SETTINGS) body.set(k, $(k).value);
  $('saveState').textContent = 'Saving...';
  try {
    const r = await fetch('/settings', { method: 'POST', body });
    if (!r.ok) throw new Error(r.status);
    showSettings(await r.json());
    $('saveState').textContent = 'Saved';
  } catch (err) {
    $('saveState').textContent = 'Save failed';
  }
}

function readNumber(id, fallback, lo, hi) {
  const v = Math.round(Number($(id).value));
  return Number.isFinite(v) && $(id).value !== '' ? Math.max(lo, Math.min(hi, v)) : fallback;
}

function updateView() {
  view.autoScale = $('autoScale').checked;
  view.markEcho = $('markEcho').checked;
  view.levelMin = readNumber('levelMin', view.levelMin, 0, ADC_MAX - 1);
  view.levelMax = readNumber('levelMax', view.levelMax, 1, ADC_MAX);
  if (view.levelMax <= view.levelMin) view.levelMax = view.levelMin + 1;
  $('levelMin').value = view.levelMin;
  $('levelMax').value = view.levelMax;
  view.cmap = $('cmap').value;
  lut = buildLut(view.cmap);
  try { localStorage.setItem('openEchoView', JSON.stringify(view)); } catch (e) {}
  render();
}

function clearPlot() {
  rows = [];
  echoes = [];
  latest = null;
  shown = 0;
  if (data) { rejectedBase = data.rejected; lateBase = data.late; }
  $('stats').textContent = 'Plot cleared; waiting for next capture.';
  render();
}

const cmapSelect = $('cmap');
Object.keys(CMAPS).forEach(k => cmapSelect.add(new Option(k, k)));
cmapSelect.value = CMAPS[view.cmap] ? view.cmap : 'viridis';
$('autoScale').checked = view.autoScale;
$('markEcho').checked = view.markEcho;
$('levelMin').value = view.levelMin;
$('levelMax').value = view.levelMax;
['autoScale', 'markEcho', 'levelMin', 'levelMax', 'cmap'].forEach(id => $(id).addEventListener('change', updateView));
SETTINGS.forEach(k => $(k).addEventListener('change', saveSettings));
$('clear').addEventListener('click', clearPlot);
$('powerOn').addEventListener('click', turnOnLuckyFinder);
window.addEventListener('resize', render);
updateView();
loadSettings();
poll();
</script>
</body>
</html>
)rawliteral";
