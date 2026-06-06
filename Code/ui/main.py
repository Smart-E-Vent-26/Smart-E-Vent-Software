import os
import sys
import time
import re
import serial
import serial.tools.list_ports
import csv
from collections import deque
import os

# Enable the built-in Qt On-Screen Virtual Keyboard for touchscreens
os.environ["QT_IM_MODULE"] = "qtvirtualkeyboard"

from PySide6.QtCore import QObject, Signal, Slot, Property, QThread, QTimer, Qt
from PySide6.QtWidgets import QApplication
from PySide6.QtQml import QQmlApplicationEngine

class SerialReader(QThread):
    """Background thread to read real Arduino telemetry without lagging the UI."""
    new_data = Signal(float, float, float, float, float) # time, pressure, volume, flow, calc_flow

    def __init__(self, port):
        super().__init__()
        self.port = port
        self.running = True
        self.start_time = time.time()
        
        # Exact regex from your telemetry_visualizer.py
        self.re_flow = re.compile(r'Flow=([-\d.]+)')
        self.re_calc_flow = re.compile(r'CalcFlow=([-\d.]+)')
        self.re_vol = re.compile(r'Vol=([-\d.]+)')
        self.re_pressure = re.compile(r'Pressure=([-\d.]+)')

    def run(self):
        try:
            # write_timeout=0.2 is the magic key that prevents the UI from freezing!
            self.arduino = serial.Serial(self.port, 115200, timeout=0.1, write_timeout=0.2)
            time.sleep(2) # Wait for arduino reset
            self.arduino.reset_input_buffer()
            print(f"[SYSTEM] Live Telemetry Thread linked to {self.port}")
        except Exception as e:
            print(f"[ERROR] Serial open failed: {e}")
            return

        while self.running and self.arduino.is_open:
            try:
                if self.arduino.in_waiting:
                    line = self.arduino.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        # Extract data via Regex
                        f_match = self.re_flow.search(line)
                        cf_match = self.re_calc_flow.search(line)
                        v_match = self.re_vol.search(line)
                        p_match = self.re_pressure.search(line)

                        if f_match and v_match:
                            flow = float(f_match.group(1))
                            calc_flow = float(cf_match.group(1)) if cf_match else 0.0
                            vol = float(v_match.group(1))
                            pressure = float(p_match.group(1)) if p_match else 0.0
                            t = time.time() - self.start_time # seconds for graph X-axis

                            self.new_data.emit(t, pressure, vol, flow, calc_flow)
            except Exception:
                pass
            time.sleep(0.005) # Yield thread

    def send_cmd(self, cmd_string):
        """Thread-safe command sending with freeze-protection."""
        if hasattr(self, 'arduino') and self.arduino and self.arduino.is_open:
            try:
                print(f"Serial Tx -> {cmd_string}")
                self.arduino.write((cmd_string + "\n").encode('utf-8'))
            except serial.SerialTimeoutException:
                print("[WARNING] Arduino busy! Dropped command to prevent UI freeze.")
            except Exception as e:
                print(f"[ERROR] Tx Failed: {e}")

class VentilatorCore(QObject):
    telemetry_updated = Signal(float, float, float, float, float) 
    ml_diagnostic_updated = Signal(str)
    
    rr_changed = Signal()
    tidal_volume_changed = Signal()
    mode_changed = Signal()
    ie_ratio_changed = Signal()
    target_pip_changed = Signal()

    is_logging_changed = Signal()
    log_filename_changed = Signal()
    log_limit_changed = Signal()

    def __init__(self):
        super().__init__()
        self._rr = 15
        self._tidal_volume = 400
        self._mode = "VCV"
        self._ie_ratio = "1:2"
        self._target_pip = 20
        
        self._is_logging = False
        self._log_filename = os.path.expanduser("~/vent_data.csv")
        self._log_limit = 10000
        self.log_buffer = deque(maxlen=self._log_limit)
        
        self.log_timer = QTimer(self)
        self.log_timer.timeout.connect(self._flush_log)
        self.log_timer.start(5000)

        self.reader = None
        self.connect_arduino()

    def connect_arduino(self):
        ports = list(serial.tools.list_ports.comports())
        for p in ports:
            if "Arduino" in p.description or "ttyACM" in p.device:
                # Initialize the threaded reader
                self.reader = SerialReader(p.device)
                self.reader.new_data.connect(self._on_telemetry_updated)
                self.reader.start()
                return
        print("[SYSTEM] No Arduino found.")

    def _on_telemetry_updated(self, t, p, v, f, cf):
        self.telemetry_updated.emit(t, p, v, f, cf)
        if self._is_logging:
            self.log_buffer.append([t, p, v, f, cf, self._mode, self._rr, self._tidal_volume, self._ie_ratio, self._target_pip])

    def _flush_log(self):
        if not self._is_logging or not self.log_buffer:
            return
        try:
            with open(self._log_filename, 'w', newline='') as file:
                writer = csv.writer(file)
                writer.writerow(["Time", "Pressure", "Volume", "PhysicalFlow", "CalcFlow", "Mode", "RR", "TV", "IERatio", "PIP"])
                writer.writerows(self.log_buffer)
        except Exception as e:
            print(f"[ERROR] Failed to save log: {e}")

    def send_command(self, cmd):
        if self.reader:
            self.reader.send_cmd(cmd)

    # --- System Control Slots ---
    @Slot()
    def startVentilation(self): self.send_command("S")
    @Slot()
    def stopVentilation(self): self.send_command("X")
    @Slot()
    def emergencyStop(self): self.send_command("E")
    @Slot()
    def calibrateHome(self): self.send_command("H")
    @Slot()
    def rebootSystem(self): self.send_command("R")
    @Slot()
    def exitApp(self): 
        print("[SYSTEM] Exiting UI from Kiosk button.")
        self.shutdown()
        os._exit(0)

    # --- Properties and Serial Translation ---
    @Property(str, notify=mode_changed)
    def mode(self): return self._mode
    @mode.setter
    def mode(self, value):
        if self._mode != value:
            self._mode = value
            self.send_command("V" if value == "VCV" else "P")
            self.mode_changed.emit()

    @Property(int, notify=rr_changed)
    def rr(self): return self._rr
    @rr.setter
    def rr(self, value):
        if self._rr != value:
            self._rr = value
            self.send_command(f"B{value}")
            self.rr_changed.emit()

    @Property(int, notify=tidal_volume_changed)
    def tidal_volume(self): return self._tidal_volume
    @tidal_volume.setter
    def tidal_volume(self, value):
        if self._tidal_volume != value:
            self._tidal_volume = value
            self.send_command(f"T{value}")
            self.tidal_volume_changed.emit()

    @Property(str, notify=ie_ratio_changed)
    def ie_ratio(self): return self._ie_ratio
    @ie_ratio.setter
    def ie_ratio(self, value):
        if self._ie_ratio != value:
            self._ie_ratio = value
            ratio_val = int(value.split(":")[1]) * 10
            self.send_command(f"R{ratio_val}")
            self.ie_ratio_changed.emit()

    @Property(int, notify=target_pip_changed)
    def target_pip(self): return self._target_pip
    @target_pip.setter
    def target_pip(self, value):
        if self._target_pip != value:
            self._target_pip = value
            self.send_command(f"I{value}")
            self.target_pip_changed.emit()

    @Property(bool, notify=is_logging_changed)
    def is_logging(self): return self._is_logging
    @is_logging.setter
    def is_logging(self, value):
        if self._is_logging != value:
            self._is_logging = value
            self.is_logging_changed.emit()
            if value: print(f"[LOG] Started logging to {self._log_filename}")

    @Property(str, notify=log_filename_changed)
    def log_filename(self): return self._log_filename
    @log_filename.setter
    def log_filename(self, value):
        if self._log_filename != value:
            self._log_filename = value
            self.log_filename_changed.emit()

    @Property(int, notify=log_limit_changed)
    def log_limit(self): return self._log_limit
    @log_limit.setter
    def log_limit(self, value):
        if self._log_limit != value:
            self._log_limit = value
            # Recreate deque with new limit, copying existing data
            new_buffer = deque(self.log_buffer, maxlen=value)
            self.log_buffer = new_buffer
            self.log_limit_changed.emit()

    def shutdown(self):
        """Gracefully stop background threads when the app closes."""
        if self.reader:
            self.reader.running = False
            self.reader.wait(1000)

if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setOverrideCursor(Qt.BlankCursor)
    engine = QQmlApplicationEngine()
    
    vent_core = VentilatorCore()
    engine.rootContext().setContextProperty("VentCore", vent_core)
    app.aboutToQuit.connect(vent_core.shutdown)
    
    current_dir = os.path.dirname(os.path.abspath(__file__))
    qml_file = os.path.join(current_dir, "dashboard.qml")
    engine.load(qml_file)
    if not engine.rootObjects():
        sys.exit(-1)
    sys.exit(app.exec())