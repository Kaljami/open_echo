# ESP32-S3 Lucky Finder web viewer

**All this ESP32-based code is purely vibe-coded slop. Do not expect things to work or be done well.**

A standalone version of the ESP32 SuperMini sketch and the [ESP32 Python viewer](../../TUSS4470_shield_002/esp32_interface.py), made for the **ESP32-S3 SuperMini**. The ESP32 captures each ping, discards non-ping triggers, finds the first echo and hosts a web page with the waveform, waterfall, distance readout and all settings. No PC software is needed; any phone or computer with a browser works.

The S3 has two cores: WiFi runs on one and the trigger interrupt and sampling on the other, so network traffic does not disturb the captures. This web sketch supports **ESP32-S3 only**.

![ESP32-S3 web ui](../../documentation/images/esp32s3_webui.png)
Moving a small object in front of the sensor, with 80cm being the depth of the container.

## Wiring

Same GPIO numbers as [ESP32_lucky_finder.ino](../ESP32_lucky_finder/ESP32_lucky_finder.ino):

| ESP32-S3 pin | Function |
|---|---|
| GPIO1 | Trigger input, a capture starts on each falling edge |
| GPIO4 | Echo signal (ADC). Use an ADC1 pin (S3: GPIO1–10); ADC2 does not work while WiFi is on. |
| GPIO5 | Optional power-button control through the NPN circuit below: HIGH presses the button, LOW releases it. |

All GPIOs are 3.3 V only. Avoid the strapping pins GPIO0, 3, 45 and 46.

### Power-button interface (4-5 V button signal)

**Never connect the Lucky Finder power-button signal directly to GPIO5.** The measured 4-5 V exceeds the ESP32-S3 GPIO voltage limits, even when configured as open-drain or input. Do not connect this signal to the S3's 3V3 or 5V supply pins either.

Use an **NPN transistor (2N3904)** as a low-side switch, with a **2.2 kOhm base resistor** and a **100 kOhm base-to-emitter pull-down**:

```text
Lucky Finder button signal (4-5 V when released)
                       |
                       C
GPIO5 ----[2.2 kOhm]----B   2N3904 (NPN)
                       E
                       |
ESP32-S3 GND ----------+---------- Lucky Finder GND

Also connect 100 kOhm from B to E.
```

1. Disconnect power from both devices before wiring.
2. Connect the transistor **emitter (E)** to the Lucky Finder ground and an ESP32-S3 **GND** pin.
3. Connect the **collector (C)** to the non-ground power-button contact, which measures 4-5 V relative to finder ground when released. This goes in parallel with the existing button; leave the physical button connected.
4. Connect **GPIO5 through 2.2 kOhm** to the **base (B)**. Connect **100 kOhm between base and emitter** so the transistor remains off while the ESP32 resets or is unpowered.
5. Check the **exact manufacturer's transistor pinout**: do not infer E/B/C from package orientation. Check for shorts before powering up.
6. With the circuit powered and idle, verify the button signal still reads approximately its original 4-5 V and GPIO5 reads approximately 0 V. During each press, GPIO5 should read approximately 3.3 V and the collector/button signal should fall close to ground (typically below 0.3 V). If it does not, disconnect power and check the wiring rather than connecting the button signal to the GPIO.

GPIO5 drives only the transistor base; the transistor handles the button's 4-5 V. This circuit is for a low-current, ground-referenced button input, **not for switching the finder's power supply**. The grounds must be common; if they cannot safely be joined, use an optocoupler interface instead. Power the ESP32 independently of the Lucky Finder's switched supply so it can turn the finder on. Change `POWER_PIN` in the sketch if using another GPIO.

## Upload

1. In the Arduino IDE, install the **esp32** board package (tested with 3.3.12).
2. Open [ESP32_lucky_finder_web.ino](ESP32_lucky_finder_web.ino) and select **Tools → Board → ESP32S3 Dev Module**.
3. Set **Tools → USB CDC On Boot → Enabled** so serial output goes over the SuperMini's USB port. The other defaults (4MB flash, PSRAM disabled) work on the SuperMini.
4. Optionally edit the WiFi settings at the top of the sketch, then upload. If the upload does not start, hold **BOOT**, tap **RST**, release **BOOT** and upload again.

Command line equivalent:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc ESP32_lucky_finder/ESP32_lucky_finder_web
arduino-cli upload  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc -p COM4 ESP32_lucky_finder/ESP32_lucky_finder_web
```

## Connecting

- **Access point (default):** join the WiFi network `OpenEcho` (password `openecho`) and open <http://192.168.4.1/>. Change `AP_PASSWORD` in the sketch for your own setup.
- **Existing network:** set `WIFI_SSID` and `WIFI_PASSWORD` in the sketch. The ESP32 then joins that network and is reachable at <http://openecho.local/> or at the IP address printed on the serial monitor. If it cannot connect within 15 s, it starts the access point instead.

## Turning on the Lucky Finder

Click **Turn on Lucky Finder** in the web interface. The external transistor grounds the power-button contact for **1 second**, releases it for **5 seconds**, grounds it for **1 second**, then releases it again. GPIO5 is HIGH during each press and LOW while released. It starts LOW; no sequence runs automatically. The 100 kOhm pull-down keeps the transistor off before firmware initialization.

The sequence is non-blocking, so capture and the web interface keep running. The button is disabled while the sequence runs, and the device rejects overlapping requests (including from other browsers) with HTTP 409. Timing is serviced by the main loop, so transitions can be slightly delayed by other work. Completion only confirms that the button sequence finished: the ESP32 cannot verify whether the Lucky Finder is on. Use this control only when the finder is off; it simulates button presses, not a guaranteed on-only command.

## Calibration

Check calibration against a known distance:

- **Ping threshold:** if the status line says "No pings accepted yet" and the discarded count keeps rising, lower it. A good value is between the first-sample levels of real pings and of noise triggers. Set it to 0 temporarily to see both in the waveform.
- **Distance per sample:** check the sample period on the status line and recalibrate against a known distance. Waterfall levels may also need adjusting.

## Settings

Detection settings are stored on the ESP32 and survive restarts. Changes apply immediately, including to the latest capture.

| Setting | Default | Description |
|---|---|---|
| Ignore first samples | 6 | Blind spot (transmit ring-down) that is never treated as an echo. |
| Distance per sample (cm) | 2.5 | `distance = sample index × distance per sample + offset`. |
| Distance offset (cm) | 10 | See above. Defaults are a rough calibration: sample 20 ≈ 60 cm, sample 30 ≈ 85 cm. |
| Echo threshold (%) | 50 | The first echo is the first peak reaching this percentage of the way from the baseline (median) to the strongest reading. Lower it to catch weaker near echoes; raise it if noise is picked up. |
| Ping threshold (0 = off) | 1700 | Captures whose first 4 samples average below this many ADC counts are discarded. This removes the extra noise trigger that arrives with every ping. |

Display settings (auto-scale, waterfall levels, colours, echo markers) are stored per browser.

The status line shows the measured **sample period**. Distance per sample is `speed of sound × sample period / 2`. For example, 34 µs in water (1480 m/s) is about 2.5 cm per sample.

## Serial output

The sketch still prints every raw capture as an `sp...` line over USB serial, so [esp32_interface.py](../../TUSS4470_shield_002/esp32_interface.py) keeps working. Set `SERIAL_STREAM` to `false` to turn this off.

## Endpoints

| Path | Description |
|---|---|
| `GET /` | Web interface |
| `GET /data?since=N` | JSON with the newest first echo, counters, sample period, `powerBusy` flag and accepted captures with sequence number greater than `N` (up to the last 8) |
| `GET /settings` | Current detection settings as JSON |
| `POST /settings` | Update settings with form fields `blind`, `dps`, `offset`, `echoPct`, `pingThr` |
| `POST /power/on` | Start the 1 s ground / 5 s release / 1 s ground sequence; returns `{"powerBusy":true}`, or HTTP 409 if already running |
