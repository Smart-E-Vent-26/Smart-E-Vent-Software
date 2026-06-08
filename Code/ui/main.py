import os
import sys
import time
import re
import serial
import serial.tools.list_ports
import csv
import pickle
import numpy as np
from collections import deque

# Enable the built-in Qt On-Screen Virtual Keyboard for touchscreens
os.environ["QT_IM_MODULE"] = "qtvirtualkeyboard"

from PySide6.QtCore import QObject, Signal, Slot, Property, QThread, QTimer, Qt
from PySide6.QtWidgets import QApplication
from PySide6.QtQml import QQmlApplicationEngine

# ── Import feature extractor from the ML sub-package ─────────────────────────
# Layout:  <project_root>/ML/pressure_classifier.py
#          <project_root>/ML/weights/pressure_models.pkl
_THIS_DIR    = os.path.dirname(os.path.abspath(__file__))
_ML_DIR      = os.path.join(_THIS_DIR,"..", "ML")
sys.path.insert(0, _ML_DIR)
from pressure_classifier import extract_pressure_features   # pure function, no side-effects

WEIGHTS_PATH = os.path.join(_ML_DIR, "weights", "pressure_models.pkl")

# ── Breath segmentation constants (must match training) ───────────────────────
VOL_RESET_THR = 5.0     # mL     — volume below this marks a breath boundary
SPIKE_THR     = 100.0   # cmH2O  — reject breath if |P| exceeds this
MIN_SEG_LEN   = 6       # samples — discard shorter segments
N_VOTE        = 3       # rolling majority-vote window (breaths)


# ═══════════════════════════════════════════════════════════════════════════════
# ML CLASSIFIER
# ═══════════════════════════════════════════════════════════════════════════════

class MLClassifier(QObject):
    """
    Accumulates live pressure + volume samples, detects each complete breath
    cycle via the Volume-reset boundary, extracts the same 20 pressure features
    used during training, runs the Random Forest pipeline, and emits the result.

    Signals
    ───────
    prediction_ready(label:str, color:str, confidence:float,
                     prob_normal:float, prob_obstr:float, prob_restr:float)
    """

    prediction_ready = Signal(str, str, float, float, float, float)

    _COLORS = {
        "Normal":      "#2ecc71",
        "Obstructive": "#e74c3c",
        "Restrictive": "#3498db",
    }

    def __init__(self, parent=None):
        super().__init__(parent)
        self._model        = None
        self._le           = None
        self._feat_cols    = None
        self._ready        = False
        self._vote_buf     = deque(maxlen=N_VOTE)
        self._breath_count = 0
        self._pres_buf     = []
        self._in_breath    = False
        self._load_weights()

    def _load_weights(self):
        if not os.path.exists(WEIGHTS_PATH):
            print(f"[ML] Weights not found: {WEIGHTS_PATH}")
            return
        try:
            with open(WEIGHTS_PATH, "rb") as f:
                w = pickle.load(f)
            self._model     = w["models"][w["best_model"]]
            self._le        = w["label_encoder"]
            self._feat_cols = w["feature_cols"]
            self._ready     = True
            print(f"[ML] Loaded '{w['best_model']}' — "
                  f"classes: {list(self._le.classes_)}, "
                  f"features: {len(self._feat_cols)}")
        except Exception as e:
            print(f"[ML] Failed to load weights: {e}")

    def push_sample(self, pressure: float, volume: float):
        """Feed one telemetry sample. Called on the main thread — very fast."""
        if not self._ready:
            return

        if volume > VOL_RESET_THR:
            self._in_breath = True
            self._pres_buf.append(pressure)
        elif self._in_breath:
            # Volume just reset → breath is complete
            self._in_breath = False
            self._classify_breath()
            self._pres_buf = []

    def _classify_breath(self):
        pres = np.array(self._pres_buf, dtype=float)

        if len(pres) < MIN_SEG_LEN:
            return
        if pres.max() > SPIKE_THR or pres.min() < -SPIKE_THR:
            print(f"[ML] Spike rejected (max={pres.max():.1f} cmH2O)")
            return

        try:
            feats = extract_pressure_features(pres)
            X     = np.array([[feats[c] for c in self._feat_cols]])
        except Exception as e:
            print(f"[ML] Feature extraction error: {e}")
            return

        try:
            enc        = self._model.predict(X)[0]
            proba      = self._model.predict_proba(X)[0]
            raw_label  = self._le.inverse_transform([enc])[0]
            confidence = float(proba[enc])
        except Exception as e:
            print(f"[ML] Inference error: {e}")
            return

        # Rolling majority vote for stability across noisy breaths
        self._vote_buf.append(raw_label)
        voted_label = raw_label
        if len(self._vote_buf) == N_VOTE:
            from collections import Counter
            voted_label = Counter(self._vote_buf).most_common(1)[0][0]

        self._breath_count += 1
        color   = self._COLORS.get(voted_label, "#ffcc00")
        classes = list(self._le.classes_)
        def _p(cls): return float(proba[classes.index(cls)]) if cls in classes else 0.0

        print(f"[ML] Breath #{self._breath_count}: {voted_label} "
              f"({confidence*100:.1f}%)  vote={list(self._vote_buf)}")

        self.prediction_ready.emit(
            voted_label, color, confidence,
            _p("Normal"), _p("Obstructive"), _p("Restrictive"),
        )


# ═══════════════════════════════════════════════════════════════════════════════
# SERIAL READER
# ═══════════════════════════════════════════════════════════════════════════════

class SerialReader(QThread):
    """Background thread to read Arduino telemetry without blocking the UI."""
    new_data = Signal(float, float, float, float, float)  # t, pressure, volume, flow, calc_flow
    state_changed = Signal(str)  # Emits new state/phase string

    def __init__(self, port):
        super().__init__()
        self.port       = port
        self.running    = True
        self.start_time = time.time()
        self.re_flow      = re.compile(r'Flow=([-\d.]+)')
        self.re_calc_flow = re.compile(r'CalcFlow=([-\d.]+)')
        self.re_vol       = re.compile(r'Vol=([-\d.]+)')
        self.re_pressure  = re.compile(r'Pressure=([-\d.]+)')

    def run(self):
        try:
            self.arduino = serial.Serial(self.port, 115200,
                                         timeout=0.1, write_timeout=0.2)
            time.sleep(2)
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
                        # Parse FSM state changes or phase labels
                        state = None
                        if "[FSM] Waiting for homing command" in line or "EMERGENCY STOPPED" in line:
                            state = "BOOT"
                        elif "[CAL] Retracting slowly" in line or "Retracting slowly" in line:
                            state = "CALIBRATING"
                        elif "System READY" in line or "[CAL] Already at home" in line or "Soft Stop Complete" in line:
                            state = "READY"
                        elif "[FSM] Ventilation STARTED" in line:
                            state = "RUNNING"
                        elif "FAULT" in line or "[FSM] FAULT" in line:
                            state = "FAULT"
                        elif "Soft Stop requested" in line:
                            state = "SOFT_STOP"
                        elif "--- INHALE ---" in line:
                            state = "INH"
                        elif "--- EXHALE ---" in line:
                            state = "EXH"
                        
                        tokens = line.split()
                        if tokens:
                            first_token = tokens[0]
                            if first_token in ["INH", "HLD", "EXH", "PAU", "S_W", "S_R"]:
                                state = first_token
                        
                        if state:
                            self.state_changed.emit(state)

                        f_match  = self.re_flow.search(line)
                        cf_match = self.re_calc_flow.search(line)
                        v_match  = self.re_vol.search(line)
                        p_match  = self.re_pressure.search(line)
                        if f_match and v_match:
                            flow      = float(f_match.group(1))
                            calc_flow = float(cf_match.group(1)) if cf_match else 0.0
                            vol       = float(v_match.group(1))
                            pressure  = float(p_match.group(1)) if p_match else 0.0
                            t         = time.time() - self.start_time
                            self.new_data.emit(t, pressure, vol, flow, calc_flow)
            except Exception:
                pass
            time.sleep(0.005)

    def send_cmd(self, cmd_string):
        if hasattr(self, 'arduino') and self.arduino and self.arduino.is_open:
            try:
                print(f"Serial Tx -> {cmd_string}")
                self.arduino.write((cmd_string + "\n").encode('utf-8'))
            except serial.SerialTimeoutException:
                print("[WARNING] Arduino busy! Dropped command.")
            except Exception as e:
                print(f"[ERROR] Tx Failed: {e}")


# ═══════════════════════════════════════════════════════════════════════════════
# VENTILATOR CORE
# ═══════════════════════════════════════════════════════════════════════════════

class VentilatorCore(QObject):

    # Raw telemetry for charts
    telemetry_updated = Signal(float, float, float, float, float)

    # ML result — consumed by the QML diagnostic panel
    # Args: label, hex_color, confidence, prob_normal, prob_obstr, prob_restr
    ml_prediction_updated = Signal(str, str, float, float, float, float)
    patient_status_changed = Signal()
    vent_state_changed = Signal()

    rr_changed           = Signal()
    tidal_volume_changed = Signal()
    mode_changed         = Signal()
    ie_ratio_changed     = Signal()
    target_pip_changed   = Signal()
    is_logging_changed   = Signal()
    log_filename_changed = Signal()
    log_limit_changed    = Signal()

    def __init__(self):
        super().__init__()
        self._rr           = 15
        self._tidal_volume = 400
        self._mode         = "VCV"
        self._ie_ratio     = "1:2"
        self._target_pip   = 20
        self._is_logging   = False
        self._log_filename = os.path.expanduser("~/vent_data.csv")
        self._log_limit    = 10000
        self.log_buffer    = deque(maxlen=self._log_limit)
        self._patient_status = "Standby"
        self._vent_state = "Boot / Uncalibrated"

        self.log_timer = QTimer(self)
        self.log_timer.timeout.connect(self._flush_log)
        self.log_timer.start(5000)

        # ML classifier — loads weights at startup, emits directly to QML signal
        self.classifier = MLClassifier(self)
        self.classifier.prediction_ready.connect(self._on_prediction_ready)

        self.reader = None
        self.connect_arduino()

    def connect_arduino(self):
        ports = list(serial.tools.list_ports.comports())
        for p in ports:
            if "Arduino" in p.description or "ttyACM" in p.device or "ttyUSB" in p.device:
                self.reader = SerialReader(p.device)
                self.reader.new_data.connect(self._on_telemetry_updated)
                self.reader.state_changed.connect(self._on_state_changed)
                self.reader.start()
                return
        print("[SYSTEM] No Arduino found.")

    def _on_prediction_ready(self, label, color, confidence, prob_normal, prob_obstr, prob_restr):
        self._patient_status = label
        self.patient_status_changed.emit()
        self.ml_prediction_updated.emit(label, color, confidence, prob_normal, prob_obstr, prob_restr)

    def _on_telemetry_updated(self, t, pressure, volume, flow, calc_flow):
        self.telemetry_updated.emit(t, pressure, volume, flow, calc_flow)
        self.classifier.push_sample(pressure, volume)          # ← ML inference
        if self._is_logging:
            self.log_buffer.append([t, pressure, volume, flow, calc_flow,
                                     self._mode, self._rr, self._tidal_volume,
                                     self._ie_ratio, self._target_pip])

    def _flush_log(self):
        if not self._is_logging or not self.log_buffer:
            return
        try:
            with open(self._log_filename, 'w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow(["Time", "Pressure", "Volume", "PhysicalFlow",
                                  "CalcFlow", "Mode", "RR", "TV", "IERatio", "PIP"])
                writer.writerows(self.log_buffer)
        except Exception as e:
            print(f"[ERROR] Failed to save log: {e}")

    def send_command(self, cmd):
        if self.reader:
            self.reader.send_cmd(cmd)

    @Slot()
    def startVentilation(self):
        self._vent_state = "Running"
        self.vent_state_changed.emit()
        self.send_command("S")
    @Slot()
    def stopVentilation(self):
        self._vent_state = "Pausing..."
        self.vent_state_changed.emit()
        self.send_command("X")
    @Slot()
    def emergencyStop(self):
        self._vent_state = "Boot / Uncalibrated"
        self.vent_state_changed.emit()
        self.send_command("E")
    @Slot()
    def calibrateHome(self):
        self._vent_state = "Calibrating"
        self.vent_state_changed.emit()
        self.send_command("H")
    @Slot()
    def exitApp(self):
        self.shutdown()
        os._exit(0)
    @Property(str, notify=patient_status_changed)
    def patient_status(self):
        return self._patient_status

    @Property(str, notify=vent_state_changed)
    def vent_state(self):
        return self._vent_state

    def _on_state_changed(self, state_code):
        mapping = {
            "BOOT": "Boot / Uncalibrated",
            "CALIBRATING": "Calibrating",
            "READY": "Ready",
            "RUNNING": "Running",
            "FAULT": "Fault",
            "SOFT_STOP": "Pausing...",
            "INH": "Inhaling (Inspiration)",
            "HLD": "Holding (Plateau)",
            "EXH": "Exhaling (Expiration)",
            "PAU": "Pausing (Expiratory Pause)",
            "S_W": "Soft Stop Wait",
            "S_R": "Soft Stop Retract"
        }
        friendly_state = mapping.get(state_code, state_code)
        if self._vent_state != friendly_state:
            self._vent_state = friendly_state
            self.vent_state_changed.emit()

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
            self.send_command(f"R{int(value.split(':')[1]) * 10}")
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
            if value: print(f"[LOG] Logging to {self._log_filename}")

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
            self.log_buffer = deque(self.log_buffer, maxlen=value)
            self.log_limit_changed.emit()

    def shutdown(self):
        if self.reader:
            self.reader.running = False
            self.reader.wait(1000)


# ═══════════════════════════════════════════════════════════════════════════════
# ENTRY POINT
# ═══════════════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setOverrideCursor(Qt.BlankCursor)
    engine = QQmlApplicationEngine()

    vent_core = VentilatorCore()
    engine.rootContext().setContextProperty("VentCore", vent_core)
    app.aboutToQuit.connect(vent_core.shutdown)

    qml_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.qml")
    engine.load(qml_file)
    if not engine.rootObjects():
        sys.exit(-1)
    sys.exit(app.exec())