"""
ml_classifier.py
================
Real-time pressure-based respiratory disease classifier.

Breath detection
----------------
Accumulates (time, pressure, volume) samples from the Arduino stream.
A breath cycle is detected when Volume rises above VOL_RISE_THR, accumulates,
then drops back below VOL_RESET_THR — identical to the segmentation used
during training in pressure_classifier.py.

On each complete breath:
  1. Extract the 20 pressure features (same function as training)
  2. Run inference with the loaded sklearn pipeline
  3. Apply a 3-breath majority vote for stability
  4. Emit prediction_ready(label, color, confidence, p_normal, p_obstr, p_restr)

Weights path
------------
Loaded from  ./ML/weights/pressure_models.pkl
relative to the directory where ventilator_core.py (this file's caller) lives.
"""

import os
import sys
import pickle
import numpy as np
from collections import deque, Counter

from PySide6.QtCore import QObject, Signal

# ── locate pressure_classifier.py (in the same ML/ dir as this file) ──────────
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _THIS_DIR)
from pressure_classifier import extract_pressure_features

# ── weights path: ./ML/weights/ relative to ventilator_core.py ────────────────
# ventilator_core.py lives one level above ML/, so we go up one dir then down
WEIGHTS_PATH = os.path.join(_THIS_DIR, "weights", "pressure_models.pkl")

# ── breath segmentation thresholds ────────────────────────────────────────────
VOL_RISE_THR   = 20.0    # mL — volume must exceed this to start a breath
VOL_RESET_THR  = 5.0     # mL — volume below this marks end of breath
MIN_BREATH_LEN = 6       # samples — shorter = artefact, skip
SPIKE_THR      = 100.0   # cmH2O — reject breath if any sample exceeds this

# ── rolling vote window ────────────────────────────────────────────────────────
N_VOTE = 3

# ── class display colours ─────────────────────────────────────────────────────
CLASS_COLOR = {
    "Normal":      "#2ecc71",
    "Obstructive": "#e74c3c",
    "Restrictive": "#3498db",
}


class MLClassifier(QObject):
    """
    Qt object that receives per-sample telemetry, segments breath cycles,
    and emits a classification after every complete breath.

    Signal: prediction_ready(label, color, confidence,
                             prob_normal, prob_obstr, prob_restr)
    """

    prediction_ready = Signal(str, str, float, float, float, float)

    def __init__(self, parent=None):
        super().__init__(parent)

        # model state
        self._model     = None
        self._le        = None
        self._feat_cols = None
        self._ready     = False
        self._load_weights()

        # breath buffer — reset at each breath boundary
        self._pres_buf   = []   # pressure samples for current breath
        self._in_breath  = False

        # rolling vote
        self._vote_buf   = deque(maxlen=N_VOTE)
        self._breath_n   = 0

    # ──────────────────────────────────────────────────────────────────────────
    # Public API  — called from VentilatorCore._on_telemetry_updated()
    # ──────────────────────────────────────────────────────────────────────────

    def push_sample(self, pressure: float, volume: float):
        """
        Feed one telemetry sample.  Call on every onTelemetry_updated event.

        Parameters
        ----------
        pressure : float   cmH2O reading from Arduino
        volume   : float   mL reading from Arduino (used only for segmentation)
        """
        if not self._ready:
            return

        if volume > VOL_RISE_THR:
            # We are inside a breath — collect pressure
            self._in_breath = True
            self._pres_buf.append(pressure)

        elif self._in_breath:
            # Volume just dropped back to ~0 → breath ended
            self._in_breath = False
            self._classify_breath()
            self._pres_buf = []     # reset for next breath

    # ──────────────────────────────────────────────────────────────────────────
    # Internal helpers
    # ──────────────────────────────────────────────────────────────────────────

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
            print(f"[ML] Loaded '{w['best_model']}' | "
                  f"classes={list(self._le.classes_)} | "
                  f"features={len(self._feat_cols)}")
        except Exception as e:
            print(f"[ML] Failed to load weights: {e}")

    def _classify_breath(self):
        pres = np.array(self._pres_buf, dtype=float)

        # ── quality checks ─────────────────────────────────────────────────
        if len(pres) < MIN_BREATH_LEN:
            return
        if pres.max() > SPIKE_THR or pres.min() < -SPIKE_THR:
            print(f"[ML] Breath #{self._breath_n + 1} rejected — spike detected")
            return

        # ── feature extraction (same function used in training) ────────────
        try:
            feats = extract_pressure_features(pres)
            X = np.array([[feats[c] for c in self._feat_cols]])
        except Exception as e:
            print(f"[ML] Feature extraction error: {e}")
            return

        # ── inference ──────────────────────────────────────────────────────
        try:
            pred_enc = self._model.predict(X)[0]
            proba    = self._model.predict_proba(X)[0]
            label    = self._le.inverse_transform([pred_enc])[0]
            conf     = float(proba[pred_enc])
        except Exception as e:
            print(f"[ML] Inference error: {e}")
            return

        # ── rolling majority vote ──────────────────────────────────────────
        self._vote_buf.append(label)
        voted_label = Counter(self._vote_buf).most_common(1)[0][0]

        self._breath_n += 1

        # per-class probabilities in fixed order
        classes = list(self._le.classes_)
        p = {c: float(proba[classes.index(c)]) for c in classes}

        color = CLASS_COLOR.get(voted_label, "#ffcc00")

        print(f"[ML] Breath #{self._breath_n}: raw={label} ({conf*100:.0f}%) "
              f"voted={voted_label} | "
              f"N={p['Normal']*100:.0f}% "
              f"O={p['Obstructive']*100:.0f}% "
              f"R={p['Restrictive']*100:.0f}%")

        self.prediction_ready.emit(
            voted_label, color, conf,
            p["Normal"], p["Obstructive"], p["Restrictive"],
        )
