# Getting Started Open Echo Interface Software

The [***Open Echo Interface***](echo_interface.py) is a cross-platform Python application that interacts with the Arduino + TUSS4470 Shield. 
It displays ultrasonic echo data in real-time using a waterfall chart visualization. 
The application is intended primarily as a testing and development tool, but is stable enough for continuous use -tested for several days on a Raspberry Pi 4 without issues.

### Key Features
- Connects to Open Echo hardware over serial or UDP
- Displays real-time and historical data in a waterfall chart
- Auto-gain to adjust the waterfall chart colors
- Shows detected depth, and (if supported) temperature and drive voltage (`vDRV`)

> **Note**  
> This interface is primarily intended for development and testing. It's proven stable, but not yet a polished end-user application.

---

## Installation & Setup

### 1. Create and activate a virtual environment

```bash
cd open_echo/TUSS4470_shield_002
python3 -m venv venv 
source venv/bin/activate 
```

### 2. Install requirements
```bash
pip install -r requirements.txt 
```

### 3. Start Open Echo Interface Software
```bash
python echo_interface.py
```

Select the correct COM port, then click Connect or press c on your keyboard. Once connected, the Open Echo board will begin streaming data, which will appear on the right side of the interface.
The red horizontal line indicates the currently detected depth, based on the strongest first echo received after the ring-down delay.

<img alt="Open Echo Interface Software" src="/documentation/images/echo_software_screenshot.jpg">


### 4. Change to your own needs

You can change different settings in the first lines of the [**Open Echo Interface**](echo_interface.py) code to customize it to your specific use cases.

### 📊 Parameter Settings

| Parameter         | Description |
|------------------|-------------|
| `BAUD_RATE`       | Must match the baud rate configured in the Arduino firmware. |
| `NUM_SAMPLES`     | Must match the `NUM_SAMPLES` value used in the Arduino firmware. |
| `MAX_ROWS`        | Sets the number of historical measurements displayed in the chart before it scrolls. |
| `Y_LABEL_DISTANCE`| Defines the vertical axis label spacing, in centimeters. |
| `SPEED_OF_SOUND`  | Used to convert sample timing into distance. Set to ~330 for air, ~1450 for water. |
| `SAMPLE_TIME`     | Sampling interval in microseconds. For the Arduino UNO with [TUSS4470_arduino.ino](arduino/TUSS4470_arduino/TUSS4470_arduino.ino), this must be set to **13.2 µs**. |

### ESP32-C3 ADC viewer

**All this ESP32-based code is purely vibe-coded slop. Do not expect things to work or be done well.**

Run `python esp32_interface.py` for the ESP32-C3 viewer, which expects 250 comma-separated ADC readings per capture. It shows the distance and sample index of the first echo after the ignored samples: the first peak that reaches **Echo threshold** percent of the way from the baseline (median reading) to the strongest reading, so stronger later bounces don't take over. Lower the threshold to catch weaker near echoes; raise it if noise is picked up. The detected sample and threshold level are shown as red lines on the waveform plot.

The default blind spot excludes indices 0–5. The initial distance calibration is approximate: `distance (cm) = sample index × 2.5 + 10`, based on sample 20 at roughly 60 cm and sample 30 at roughly 85 cm. Adjust the ignored sample count, distance per sample, and offset for your hardware and medium; the ESP32 data does not provide timing or calibration information. Changes apply immediately to the latest capture.

The ESP32 can trigger twice per ping, where the second capture is noise without a transmit pulse. Real pings start with a strong ring-down, so captures whose first 4 samples average below **Ping threshold** (default 1700 ADC counts) are discarded and counted. Set it to 0 to show every capture.

For a standalone version where the ESP32 does the processing and serves this interface over WiFi, see [ESP32_lucky_finder_web](../ESP32_lucky_finder/ESP32_lucky_finder_web/README.md).

--- 
Want to stay updated, have questions or want to participate? Join my [Discord](https://discord.com/invite/rerCyqAcrw)!

Or write an issue. Thanks!
