/*
  ESP32-S3 SuperMini - Open Echo standalone web viewer

  Captures an ADC trace on every falling edge of the trigger input, discards captures
  without a transmit ring-down (non-ping triggers), finds the first echo and serves a
  web interface with a live waveform, waterfall, distance readout and all settings.

  Board: "ESP32S3 Dev Module" with "USB CDC On Boot: Enabled" for USB serial output.
  Also builds for the ESP32-C3 SuperMini ("ESP32C3 Dev Module"). See README.md for setup.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <math.h>
#include "echo_processing.h"
#include "web_page.h"

// ---------------------- WiFi ----------------------
// Leave WIFI_SSID empty to always start an access point instead of joining a network.
const char *WIFI_SSID = "";
const char *WIFI_PASSWORD = "";
const char *AP_SSID = "OpenEcho";
const char *AP_PASSWORD = "openecho";  // at least 8 characters, or "" for an open network
const char *HOSTNAME = "openecho";      // http://openecho.local
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// ---------------------- Hardware ----------------------
const int INTERRUPT_PIN = 1;  // GPIO1: trigger input (falling edge)
const int ANALOG_PIN = 4;     // GPIO4: echo signal; must be an ADC1 pin (S3: GPIO1-10, C3: GPIO0-4) because ADC2 is unavailable with WiFi on
const int NUM_SAMPLES = 250;

// ---------------------- Processing ----------------------
const size_t RINGDOWN_SAMPLES = 4;              // samples averaged for the ping threshold
const uint32_t MAX_TRIGGER_LATENCY_US = 2000;   // captures that could not start this soon are skipped
const bool SERIAL_STREAM = true;                // also print "sp..." lines for esp32_interface.py
const int HISTORY = 8;                          // captures buffered for the browser between polls

struct Settings {
  int blindSamples = 6;
  float distancePerSample = 2.5f;  // cm
  float distanceOffset = 10.0f;    // cm
  int echoThresholdPercent = 50;
  int pingThreshold = 1700;        // ADC counts, 0 = off
};

struct Capture {
  uint32_t seq;
  int16_t echoIndex;
  uint16_t values[NUM_SAMPLES];
};

// State shared between the capture task and the web server, guarded by stateMutex.
SemaphoreHandle_t stateMutex;
Settings settings;
Capture history[HISTORY];
uint32_t captureSeq = 0;  // number of accepted captures; capture N is in history[(N - 1) % HISTORY]
uint32_t rejectedCount = 0;
uint32_t lateCount = 0;
float samplePeriodUs = 0;
EchoResult latestEcho = {false, -1, 0};
uint16_t serialValues[NUM_SAMPLES];
bool serialPending = false;

TaskHandle_t captureTaskHandle = nullptr;
volatile uint32_t triggerUs = 0;

WebServer server(80);
Preferences prefs;

// ---------------------- Settings ----------------------

float clampFloat(float value, float lo, float hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

int clampInt(float value, int lo, int hi) {
  return (int)lroundf(clampFloat(value, lo, hi));
}

Settings sanitize(Settings s) {
  s.blindSamples = clampInt(s.blindSamples, 0, NUM_SAMPLES - 1);
  s.distancePerSample = isfinite(s.distancePerSample) ? clampFloat(s.distancePerSample, 0.001f, 1000.0f) : Settings().distancePerSample;
  s.distanceOffset = isfinite(s.distanceOffset) ? clampFloat(s.distanceOffset, -10000.0f, 10000.0f) : Settings().distanceOffset;
  s.echoThresholdPercent = clampInt(s.echoThresholdPercent, 1, 100);
  s.pingThreshold = clampInt(s.pingThreshold, 0, 4095);
  return s;
}

void loadSettings() {
  Settings defaults;
  Settings s;
  s.blindSamples = prefs.getInt("blind", defaults.blindSamples);
  s.distancePerSample = prefs.getFloat("dps", defaults.distancePerSample);
  s.distanceOffset = prefs.getFloat("offset", defaults.distanceOffset);
  s.echoThresholdPercent = prefs.getInt("echoPct", defaults.echoThresholdPercent);
  s.pingThreshold = prefs.getInt("pingThr", defaults.pingThreshold);
  settings = sanitize(s);
}

void storeSettings(const Settings &s) {
  prefs.putInt("blind", s.blindSamples);
  prefs.putFloat("dps", s.distancePerSample);
  prefs.putFloat("offset", s.distanceOffset);
  prefs.putInt("echoPct", s.echoThresholdPercent);
  prefs.putInt("pingThr", s.pingThreshold);
}

// Re-runs detection on the newest capture so setting changes show immediately. Hold stateMutex.
void recomputeLatestEcho() {
  static uint16_t scratch[NUM_SAMPLES];
  if (captureSeq == 0) {
    return;
  }
  const Capture &latest = history[(captureSeq - 1) % HISTORY];
  latestEcho = findFirstEcho(latest.values, NUM_SAMPLES, settings.blindSamples, settings.echoThresholdPercent, scratch);
}

// ---------------------- Capture ----------------------

void IRAM_ATTR onTrigger() {
  triggerUs = micros();
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(captureTaskHandle, &woken);
  portYIELD_FROM_ISR(woken);
}

void captureTask(void *) {
  static uint16_t samples[NUM_SAMPLES];
  static uint16_t scratch[NUM_SAMPLES];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t start = micros();
    if (start - triggerUs > MAX_TRIGGER_LATENCY_US) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      lateCount++;
      xSemaphoreGive(stateMutex);
      continue;
    }

    for (int i = 0; i < NUM_SAMPLES; i++) {
      samples[i] = analogRead(ANALOG_PIN);
    }
    uint32_t elapsed = micros() - start;
    ulTaskNotifyTake(pdTRUE, 0);  // drop triggers that fired during the capture

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    samplePeriodUs = (float)elapsed / NUM_SAMPLES;
    if (SERIAL_STREAM) {
      memcpy(serialValues, samples, sizeof(samples));
      serialPending = true;
    }
    if (isPing(samples, NUM_SAMPLES, RINGDOWN_SAMPLES, settings.pingThreshold)) {
      captureSeq++;
      Capture &slot = history[(captureSeq - 1) % HISTORY];
      slot.seq = captureSeq;
      memcpy(slot.values, samples, sizeof(samples));
      latestEcho = findFirstEcho(samples, NUM_SAMPLES, settings.blindSamples, settings.echoThresholdPercent, scratch);
      slot.echoIndex = latestEcho.found ? latestEcho.index : -1;
    } else {
      rejectedCount++;
    }
    xSemaphoreGive(stateMutex);
  }
}

void printPendingCapture() {
  static uint16_t values[NUM_SAMPLES];
  static char line[NUM_SAMPLES * 6 + 8];
  bool pending;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pending = serialPending;
  if (pending) {
    memcpy(values, serialValues, sizeof(values));
    serialPending = false;
  }
  xSemaphoreGive(stateMutex);
  if (!pending || !Serial) {
    return;
  }

  size_t len = 0;
  line[len++] = 's';
  line[len++] = 'p';
  for (int i = 0; i < NUM_SAMPLES; i++) {
    if (i) {
      line[len++] = ',';
      line[len++] = ' ';
    }
    len += snprintf(line + len, sizeof(line) - len, "%u", (unsigned)values[i]);
  }
  line[len++] = '\r';
  line[len++] = '\n';
  Serial.write((const uint8_t *)line, len);
}

// ---------------------- Web server ----------------------

void appendSettingsJson(String &json, const Settings &s) {
  json += "{\"blind\":";
  json += s.blindSamples;
  json += ",\"dps\":";
  json += String(s.distancePerSample, 3);
  json += ",\"offset\":";
  json += String(s.distanceOffset, 2);
  json += ",\"echoPct\":";
  json += s.echoThresholdPercent;
  json += ",\"pingThr\":";
  json += s.pingThreshold;
  json += ",\"samples\":";
  json += NUM_SAMPLES;
  json += "}";
}

void sendJson(const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
}

void handleData() {
  static Capture copies[HISTORY];
  uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), nullptr, 10) : 0;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  uint32_t seq = captureSeq;
  uint32_t rejected = rejectedCount;
  uint32_t late = lateCount;
  float period = samplePeriodUs;
  EchoResult echo = latestEcho;
  Settings s = settings;
  int echoCounts = (seq > 0 && echo.found) ? history[(seq - 1) % HISTORY].values[echo.index] : 0;
  uint32_t oldest = seq > (uint32_t)HISTORY ? seq - HISTORY + 1 : 1;
  uint32_t first = since >= seq ? seq + 1 : max(since + 1, oldest);
  int copyCount = 0;
  for (uint32_t q = first; q <= seq; q++) {
    copies[copyCount++] = history[(q - 1) % HISTORY];
  }
  xSemaphoreGive(stateMutex);

  String json;
  json.reserve(256 + copyCount * (NUM_SAMPLES * 5 + 32));
  json += "{\"seq\":";
  json += seq;
  json += ",\"rejected\":";
  json += rejected;
  json += ",\"late\":";
  json += late;
  json += ",\"samplePeriodUs\":";
  json += String(period, 2);
  json += ",\"echo\":";
  if (seq > 0 && echo.found) {
    json += "{\"index\":";
    json += echo.index;
    json += ",\"level\":";
    json += String(echo.level, 1);
    json += ",\"distance\":";
    json += String(echo.index * s.distancePerSample + s.distanceOffset, 2);
    json += ",\"counts\":";
    json += echoCounts;
    json += "}";
  } else {
    json += "null";
  }
  json += ",\"captures\":[";
  for (int c = 0; c < copyCount; c++) {
    if (c) {
      json += ',';
    }
    json += "{\"seq\":";
    json += copies[c].seq;
    json += ",\"echo\":";
    json += copies[c].echoIndex;
    json += ",\"v\":[";
    for (int i = 0; i < NUM_SAMPLES; i++) {
      if (i) {
        json += ',';
      }
      json += (unsigned int)copies[c].values[i];
    }
    json += "]}";
  }
  json += "]}";
  sendJson(json);
}

void handleGetSettings() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  Settings s = settings;
  xSemaphoreGive(stateMutex);
  String json;
  appendSettingsJson(json, s);
  sendJson(json);
}

bool readArg(const char *name, float &out) {
  if (!server.hasArg(name)) {
    return false;
  }
  String text = server.arg(name);
  text.trim();
  if (text.length() == 0) {
    return false;
  }
  char *end;
  float value = strtof(text.c_str(), &end);
  if (*end != '\0' || !isfinite(value)) {
    return false;
  }
  out = value;
  return true;
}

void handlePostSettings() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  Settings s = settings;
  xSemaphoreGive(stateMutex);

  float value;
  if (readArg("blind", value)) s.blindSamples = clampInt(value, 0, NUM_SAMPLES - 1);
  if (readArg("dps", value)) s.distancePerSample = value;
  if (readArg("offset", value)) s.distanceOffset = value;
  if (readArg("echoPct", value)) s.echoThresholdPercent = clampInt(value, 1, 100);
  if (readArg("pingThr", value)) s.pingThreshold = clampInt(value, 0, 4095);
  s = sanitize(s);

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  settings = s;
  recomputeLatestEcho();
  xSemaphoreGive(stateMutex);
  storeSettings(s);

  String json;
  appendSettingsJson(json, s);
  sendJson(json);
}

// ---------------------- Setup ----------------------

void startWifi() {
  WiFi.setHostname(HOSTNAME);
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setSleep(false);
      Serial.printf("Connected, open http://%s/ or http://%s.local/\r\n", WiFi.localIP().toString().c_str(), HOSTNAME);
      return;
    }
    Serial.println("WiFi connection failed, starting access point instead");
    WiFi.disconnect(true);
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, strlen(AP_PASSWORD) ? AP_PASSWORD : nullptr);
  Serial.printf("Access point '%s' started, open http://%s/\r\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Open Echo ESP32 web viewer starting");

  stateMutex = xSemaphoreCreateMutex();
  prefs.begin("openecho", false);
  loadSettings();

  pinMode(INTERRUPT_PIN, INPUT);
  pinMode(ANALOG_PIN, INPUT);
  analogReadResolution(12);  // 0-4095
  analogRead(ANALOG_PIN);    // configure the ADC channel before the first capture

  // Highest priority so a capture starts right after the trigger; each capture lasts only milliseconds.
  // On dual-core chips (S3) it shares the Arduino core with the trigger interrupt, while WiFi runs on the other core.
#if CONFIG_FREERTOS_UNICORE
  xTaskCreate(captureTask, "capture", 4096, nullptr, configMAX_PRIORITIES - 1, &captureTaskHandle);
#else
  xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, configMAX_PRIORITIES - 1, &captureTaskHandle, xPortGetCoreID());
#endif
  attachInterrupt(digitalPinToInterrupt(INTERRUPT_PIN), onTrigger, FALLING);

  startWifi();
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/settings", HTTP_GET, handleGetSettings);
  server.on("/settings", HTTP_POST, handlePostSettings);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  server.handleClient();
  if (SERIAL_STREAM) {
    printPendingCapture();
  }
  delay(2);
}
