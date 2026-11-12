import sys
import time
import socket
import serial
import serial.tools.list_ports
import numpy as np
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QVBoxLayout, QWidget, QComboBox,
    QPushButton, QLabel, QHBoxLayout, QCheckBox, QSpinBox, QGroupBox,
    QFormLayout, QDoubleSpinBox
)
from PyQt5.QtCore import QThread, pyqtSignal
from PyQt5.QtGui import QPalette, QColor
import pyqtgraph as pg
import qdarktheme

# ESP32-C3 firmware line format:
# sp343, 390, 412, ... (exactly 250 ADC readings)
BAUD_RATE = 115200
NUM_SAMPLES = 250
MAX_ROWS = 300
ADC_MAX = 4095
# Real pings start with the transmit ring-down; extra triggers without a ping do not.
RINGDOWN_SAMPLES = 4
DEFAULT_PING_THRESHOLD = 1700
DEFAULT_ECHO_THRESHOLD_PERCENT = 50


def get_serial_ports():
    return [p.device for p in serial.tools.list_ports.comports()]


def find_first_echo(values, start, threshold_percent):
    """Return (sample index, threshold level) of the first echo peak after `start`, or None.

    The threshold sits `threshold_percent` of the way from the baseline (median) to the
    strongest reading, so it adapts to the ADC offset. Once a sample crosses it, the
    rising edge is followed to its local peak.
    """
    segment = values[start:]
    if segment.size == 0:
        return None
    baseline = float(np.median(segment))
    peak = float(segment.max())
    if peak <= baseline:
        return None
    level = baseline + (peak - baseline) * threshold_percent / 100.0
    i = int(np.argmax(segment >= level))
    while i + 1 < segment.size and segment[i + 1] >= segment[i]:
        i += 1
    return start + i, level


def parse_esp32_line(line):
    line = line.strip()
    if not line.startswith("sp"):
        return None
    try:
        values = [int(part.strip()) for part in line[2:].strip().split(",")]
    except ValueError:
        return None
    if len(values) != NUM_SAMPLES or any(v < 0 or v > ADC_MAX for v in values):
        return None
    return np.asarray(values, dtype=np.float32)


class SerialReader(QThread):
    data_received = pyqtSignal(object)
    status = pyqtSignal(str)

    def __init__(self, port, baud_rate=BAUD_RATE):
        super().__init__()
        self.port = port
        self.baud_rate = baud_rate
        self.running = True

    def run(self):
        try:
            with serial.Serial(self.port, self.baud_rate, timeout=1) as ser:
                time.sleep(1.2)
                ser.reset_input_buffer()
                self.status.emit(f"Connected to {self.port} at {self.baud_rate} baud")
                while self.running:
                    raw = ser.readline()
                    if raw:
                        values = parse_esp32_line(raw.decode("ascii", errors="ignore"))
                        if values is not None:
                            self.data_received.emit(values)
        except serial.SerialException as exc:
            self.status.emit(f"Serial error: {exc}")

    def stop(self):
        self.running = False
        self.wait(1500)


class WaterfallApp(QMainWindow):
    def __init__(self):
        super().__init__()
        self.reader = None
        self.row_count = 0
        self.capture_count = 0
        self.rejected_count = 0
        self.data = np.full((MAX_ROWS, NUM_SAMPLES), np.nan, dtype=np.float32)
        self.last_values = None

        self.setWindowTitle("ESP32-C3 ADC Viewer — Waterfall + Waveform")
        self.resize(1200, 800)

        palette = self.palette()
        palette.setColor(QPalette.Window, QColor("#242424"))
        self.setPalette(palette)
        self.setAutoFillBackground(True)

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # Top: latest single capture line plot
        self.wave_plot = pg.PlotWidget(title="Latest capture")
        self.wave_plot.setLabel("bottom", "Sample index")
        self.wave_plot.setLabel("left", "ADC reading", units="counts")
        self.wave_plot.showGrid(x=True, y=True, alpha=0.25)
        self.wave_plot.setXRange(0, NUM_SAMPLES - 1, padding=0)
        self.wave_plot.setYRange(0, ADC_MAX, padding=0)
        self.wave_curve = self.wave_plot.plot(pen=pg.mkPen(width=1.5))
        self.echo_marker = pg.InfiniteLine(angle=90, pen=pg.mkPen("r", width=1.5))
        self.echo_level = pg.InfiniteLine(angle=0, pen=pg.mkPen("r", width=1, style=pg.QtCore.Qt.DashLine))
        for line in (self.echo_marker, self.echo_level):
            line.hide()
            self.wave_plot.addItem(line)
        root.addWidget(self.wave_plot, stretch=2)

        # Waterfall
        self.water_plot = pg.PlotWidget(title="Waterfall (each row is one trigger/capture)")
        self.water_plot.setLabel("bottom", "Sample index")
        self.water_plot.setLabel("left", "Capture number (newest at bottom)")
        self.water_plot.showGrid(x=False, y=False)
        self.water_plot.setMouseEnabled(x=False, y=False)
        self.water_plot.invertY(True)
        root.addWidget(self.water_plot, stretch=3)

        self.image = pg.ImageItem(axisOrder="row-major")
        self.water_plot.addItem(self.image)
        self.image.setLookupTable(pg.colormap.get("CET-L9").getLookupTable())
        self.image.setImage(self.data, autoLevels=False, levels=(0, ADC_MAX))
        self.image.setRect(pg.QtCore.QRectF(0, 0, NUM_SAMPLES, MAX_ROWS))

        # Controls
        controls = QHBoxLayout()
        controls.addWidget(QLabel("Serial port:"))
        self.port_combo = QComboBox()
        self.port_combo.addItems(get_serial_ports())
        controls.addWidget(self.port_combo, stretch=1)

        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.clicked.connect(self.refresh_ports)
        controls.addWidget(self.refresh_button)

        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self.toggle_serial)
        controls.addWidget(self.connect_button)

        self.gradient_combo = QComboBox()
        self.gradient_combo.addItems(["viridis", "plasma", "inferno", "magma", "thermal", "flame", "grey"])
        self.gradient_combo.currentTextChanged.connect(self.set_gradient)
        controls.addWidget(self.gradient_combo)
        root.addLayout(controls)

        calibration = QHBoxLayout()
        calibration.addWidget(QLabel("Ignore first samples:"))
        self.blind_samples = QSpinBox()
        self.blind_samples.setRange(0, NUM_SAMPLES - 1)
        self.blind_samples.setValue(6)
        self.blind_samples.valueChanged.connect(self.update_strongest)
        calibration.addWidget(self.blind_samples)

        calibration.addWidget(QLabel("Distance per sample (cm):"))
        self.distance_per_sample = QDoubleSpinBox()
        self.distance_per_sample.setDecimals(3)
        self.distance_per_sample.setRange(0.001, 1000)
        self.distance_per_sample.setValue(2.5)
        self.distance_per_sample.valueChanged.connect(self.update_strongest)
        calibration.addWidget(self.distance_per_sample)

        calibration.addWidget(QLabel("Distance offset (cm):"))
        self.distance_offset = QDoubleSpinBox()
        self.distance_offset.setDecimals(2)
        self.distance_offset.setRange(-10000, 10000)
        self.distance_offset.setValue(10)
        self.distance_offset.valueChanged.connect(self.update_strongest)
        calibration.addWidget(self.distance_offset)

        calibration.addWidget(QLabel("Echo threshold (%):"))
        self.echo_threshold = QSpinBox()
        self.echo_threshold.setRange(1, 100)
        self.echo_threshold.setValue(DEFAULT_ECHO_THRESHOLD_PERCENT)
        self.echo_threshold.setToolTip(
            "First echo = first peak reaching this percentage of the way from the "
            "baseline to the strongest reading. Lower it to catch weaker near echoes."
        )
        self.echo_threshold.valueChanged.connect(self.update_strongest)
        calibration.addWidget(self.echo_threshold)
        calibration.addWidget(QLabel("Ping threshold (0 = off):"))
        self.ping_threshold = QSpinBox()
        self.ping_threshold.setRange(0, ADC_MAX)
        self.ping_threshold.setValue(DEFAULT_PING_THRESHOLD)
        self.ping_threshold.setToolTip(
            f"Captures whose first {RINGDOWN_SAMPLES} samples average below this are "
            "treated as non-ping triggers and discarded."
        )
        calibration.addWidget(self.ping_threshold)
        root.addLayout(calibration)

        options = QHBoxLayout()
        self.auto_scale = QCheckBox("Auto-scale each capture (waveform)")
        self.auto_scale.setChecked(True)
        self.auto_scale.stateChanged.connect(self.update_waveform)
        options.addWidget(self.auto_scale)

        options.addWidget(QLabel("Waterfall min:"))
        self.level_min = QSpinBox()
        self.level_min.setRange(0, ADC_MAX - 1)
        self.level_min.setValue(300)
        self.level_min.valueChanged.connect(self.update_levels)
        options.addWidget(self.level_min)

        options.addWidget(QLabel("Waterfall max:"))
        self.level_max = QSpinBox()
        self.level_max.setRange(1, ADC_MAX)
        self.level_max.setValue(600)
        self.level_max.valueChanged.connect(self.update_levels)
        options.addWidget(self.level_max)

        self.clear_button = QPushButton("Clear plot")
        self.clear_button.clicked.connect(self.clear_plot)
        options.addWidget(self.clear_button)
        root.addLayout(options)

        self.status_label = QLabel("Connect to the ESP32-C3. Expected format: 'sp' + 250 comma-separated ADC values.")
        self.status_label.setWordWrap(True)
        root.addWidget(self.status_label)
        self.strongest_label = QLabel("First echo: waiting for capture.")
        root.addWidget(self.strongest_label)
        self.latest_label = QLabel("No captures received yet.")
        root.addWidget(self.latest_label)

    def refresh_ports(self):
        current = self.port_combo.currentText()
        self.port_combo.clear()
        ports = get_serial_ports()
        self.port_combo.addItems(ports)
        if current in ports:
            self.port_combo.setCurrentText(current)

    def toggle_serial(self):
        if self.reader and self.reader.isRunning():
            self.reader.stop()
            self.reader = None
            self.connect_button.setText("Connect")
            self.status_label.setText("Disconnected")
            return

        port = self.port_combo.currentText().strip()
        if not port:
            self.status_label.setText("No serial port found. Connect the ESP32-C3 and click Refresh.")
            return

        self.reader = SerialReader(port)
        self.reader.data_received.connect(self.on_samples)
        self.reader.status.connect(self.status_label.setText)
        self.reader.start()
        self.connect_button.setText("Disconnect")

    def is_ping(self, values):
        threshold = self.ping_threshold.value()
        return threshold == 0 or float(values[:RINGDOWN_SAMPLES].mean()) >= threshold

    def on_samples(self, values):
        if not self.is_ping(values):
            self.rejected_count += 1
            return
        self.last_values = values.copy()
        self.capture_count += 1

        self.wave_curve.setData(np.arange(NUM_SAMPLES), values)
        if self.auto_scale.isChecked():
            lo = float(np.min(values))
            hi = float(np.max(values))
            margin = max(5.0, (hi - lo) * 0.12)
            if hi - lo < 1:
                margin = 5.0
            self.wave_plot.setYRange(max(0, lo - margin), min(ADC_MAX, hi + margin), padding=0)
        else:
            self.wave_plot.setYRange(0, ADC_MAX, padding=0)

        self.data[:-1, :] = self.data[1:, :]
        self.data[-1, :] = values
        self.row_count = min(self.row_count + 1, MAX_ROWS)
        self.image.setImage(self.data, autoLevels=False, levels=(self.level_min.value(), self.level_max.value()))
        self.latest_label.setText(
            f"Capture {self.capture_count} | min {int(values.min())} | max {int(values.max())} | "
            f"mean {float(values.mean()):.1f} | peak-to-peak {int(values.max()-values.min())} counts | "
            f"{self.rejected_count} non-ping captures discarded"
        )
        self.update_strongest()

    def update_strongest(self, *_):
        if self.last_values is None:
            return
        echo = find_first_echo(self.last_values, self.blind_samples.value(), self.echo_threshold.value())
        if echo is None:
            self.echo_marker.hide()
            self.echo_level.hide()
            self.strongest_label.setText("First echo: none found above the baseline.")
            return
        index, level = echo
        self.echo_marker.setPos(index)
        self.echo_level.setPos(level)
        self.echo_marker.show()
        self.echo_level.show()
        distance = index * self.distance_per_sample.value() + self.distance_offset.value()
        if distance < 0:
            self.strongest_label.setText(
                f"First echo: sample {index}; negative distance, check calibration."
            )
        else:
            self.strongest_label.setText(
                f"First echo: {distance:.1f} cm | sample {index} | "
                f"{int(self.last_values[index])} counts"
            )

    def update_waveform(self, *_):
        if self.last_values is None:
            return
        values = self.last_values
        self.wave_curve.setData(np.arange(NUM_SAMPLES), values)
        if self.auto_scale.isChecked():
            lo, hi = float(values.min()), float(values.max())
            margin = max(5.0, (hi - lo) * 0.12)
            self.wave_plot.setYRange(max(0, lo-margin), min(ADC_MAX, hi+margin), padding=0)
        else:
            self.wave_plot.setYRange(0, ADC_MAX, padding=0)

    def update_levels(self, *_):
        if self.level_max.value() <= self.level_min.value():
            self.level_max.blockSignals(True)
            self.level_max.setValue(min(ADC_MAX, self.level_min.value() + 1))
            self.level_max.blockSignals(False)
        self.image.setLevels((self.level_min.value(), self.level_max.value()))

    def set_gradient(self, name):
        try:
            self.image.setLookupTable(pg.colormap.get(name, source="matplotlib").getLookupTable())
        except Exception:
            pass

    def clear_plot(self):
        self.data[:] = np.nan
        self.row_count = 0
        self.capture_count = 0
        self.rejected_count = 0
        self.last_values = None
        self.wave_curve.setData([], [])
        self.wave_plot.setYRange(0, ADC_MAX, padding=0)
        self.image.setImage(self.data, autoLevels=False, levels=(self.level_min.value(), self.level_max.value()))
        self.strongest_label.setText("First echo: waiting for capture.")
        self.echo_marker.hide()
        self.echo_level.hide()
        self.latest_label.setText("Plot cleared; waiting for next capture.")

    def closeEvent(self, event):
        if self.reader:
            self.reader.stop()
        event.accept()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    qdarktheme.setup_theme("dark")
    window = WaterfallApp()
    window.show()
    sys.exit(app.exec())
