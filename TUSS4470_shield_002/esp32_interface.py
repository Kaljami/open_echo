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
# sp343, 390, 412, ... (exactly 300 ADC readings)
BAUD_RATE = 115200
NUM_SAMPLES = 250
MAX_ROWS = 300
ADC_MAX = 4095


def get_serial_ports():
    return [p.device for p in serial.tools.list_ports.comports()]


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

        self.status_label = QLabel("Connect to the ESP32-C3. Expected format: 'sp' + 300 comma-separated ADC values.")
        self.status_label.setWordWrap(True)
        root.addWidget(self.status_label)
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

    def on_samples(self, values):
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
            f"mean {float(values.mean()):.1f} | peak-to-peak {int(values.max()-values.min())} counts"
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
        self.last_values = None
        self.wave_curve.setData([], [])
        self.wave_plot.setYRange(0, ADC_MAX, padding=0)
        self.image.setImage(self.data, autoLevels=False, levels=(self.level_min.value(), self.level_max.value()))
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
