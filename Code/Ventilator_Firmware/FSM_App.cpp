// ===========================================================
// FSM_App.cpp — Application Layer: Ventilator State Machine
// Smart E-Ventilator Firmware v2.0
//
// Breath cycle:
//   INHALE → HOLD → EXHALE → PAUSE → (repeat)
//
// Key changes from v1.0:
//   - User-triggered homing (boot waits for 'H' command)
//   - S-Curve motion profiling on BOTH inhale and exhale
//   - Inspiratory hold (plateau) phase
//   - Expiratory pause phase
//   - Step-based volume (not flow-integrated)
//   - Separate inhale/exhale cruise speeds (I:E ratio)
//   - Dynamic kinematics from clinical settings
//   - BMP280 differential pressure
// ===========================================================
#include "FSM_App.h"
#include "HAL_Board.h"
#include "HAL_Motor.h"
#include "HAL_Sensors.h"
#include "Kinematics.h"
#include "Filters.h"
#include "Safety.h"

// =============================================================
// COMPILE-TIME DEFAULTS
// =============================================================
#define DEFAULT_BPM                 15
#define DEFAULT_IE_RATIO            2.0f      // 1:2
#define DEFAULT_TIDAL_ML            400.0f    // 400 mL
#define DEFAULT_TARGET_PIP_KPA      2.5f      // ~25 cmH2O
#define SENSOR_POLL_INTERVAL_MS     40        // ~25 Hz (matches test code 40ms window)
#define TELEMETRY_PRINT_INTERVAL_MS 250       // Print to Serial every 250ms

// =============================================================
// PRIVATE STATE
// =============================================================
static VentState    _state            = STATE_BOOT;
static VentMode     _mode             = MODE_VCV;
static VentSettings _settings;
static EMA_Filter   _flowFilter;

// Timing helpers
static uint32_t _stateEntryMs    = 0;
static uint32_t _lastSensorMs    = 0;
static uint32_t _lastTelemetryMs = 0;

// Computed per breath
static uint32_t _inhaleTimeMs    = 0;
static uint32_t _exhaleTimeMs    = 0;

// Per-breath bookkeeping
static int32_t  _currentInhaleSteps = 0;
static float    _currentPressureKpa = 0.0f;
static float    _peakPressureKpa    = 0.0f; // Track PIP for Disconnect Alarm
static float    _currentFlowLPM     = 0.0f;
static uint32_t _holdDurationMs     = 0;   // Inspiratory hold duration
static uint32_t _pauseDurationMs    = 0;   // Expiratory pause duration

// Output mode
static bool     _graphMode          = false;

// Calibration sub-state
static bool     _calibRetracting    = false;

// Hardware Stop request
static bool     _stopRequested      = false;

// =============================================================
// HELPER: recompute breath phase timing from current settings
// =============================================================
static void _computeBreathTiming() {
    uint32_t breathPeriodMs = 60000UL / _settings.bpm;
    float    totalRatio     = 1.0f + _settings.ieRatio;
    _inhaleTimeMs = (uint32_t)(breathPeriodMs / totalRatio);
    _exhaleTimeMs = breathPeriodMs - _inhaleTimeMs;
}

// =============================================================
// HELPER: recalculate kinematics from current clinical settings
// =============================================================
static void _updateKinematics() {
    Kin_UpdateDynamics(_settings.bpm, _settings.ieRatio,
                       _settings.targetTidalVolume_mL);
    _computeBreathTiming();
}

// =============================================================
// HELPER: begin a new inhale phase
// =============================================================
static void _startInhale() {
    _stateEntryMs = HAL_GetMillis();
    _peakPressureKpa = 0.0f; // Reset peak tracker for the new breath

    _updateKinematics();

    int32_t targetSteps;
    if (_mode == MODE_VCV) {
        targetSteps = Kin_GetStrokeSteps();
        if (targetSteps > _settings.maxCompressSteps)
            targetSteps = _settings.maxCompressSteps;
    } else {
        // PCV: plan full stroke; FSM will stop early on pressure target
        targetSteps = _settings.maxCompressSteps;
    }

    HAL_Motor_Enable();
    HAL_Motor_SetDirection(MOTOR_DIR_COMPRESS);
    Kin_PlanInhale(targetSteps);

    Safety_SetLEDs(true, false, false);     // Green = inhaling
    _state        = STATE_INHALE;
    _stateEntryMs = HAL_GetMillis();

    if (!_graphMode) {
        Serial.println(F("\n--- INHALE ---"));
        Serial.print(F("  Target: "));
        Serial.print(_settings.targetTidalVolume_mL, 0);
        Serial.print(F(" mL (")); Serial.print(targetSteps);
        Serial.print(F(" steps)  Cruise: "));
        Serial.print(Kin_GetInhaleCruiseUs());
        Serial.println(F("us"));
    }
}

// =============================================================
// FSM_Init
// =============================================================
void FSM_Init() {
    _state = STATE_BOOT;
    _mode  = MODE_VCV;

    _settings.bpm                = DEFAULT_BPM;
    _settings.ieRatio            = DEFAULT_IE_RATIO;
    _settings.targetTidalVolume_mL = DEFAULT_TIDAL_ML;
    _settings.targetPIP_kPa      = DEFAULT_TARGET_PIP_KPA;
    _settings.maxCompressSteps   = MECH_FULL_COMPRESS_STEPS;  // Default until calibrated

    Filter_EMA_Init(&_flowFilter, 0.15f);
    _updateKinematics();
    _stateEntryMs     = HAL_GetMillis();
    _calibRetracting  = false;

    Serial.println(F("[FSM] Waiting for homing command (H)..."));
}

// =============================================================
// FSM_Update — Called every loop() iteration
// =============================================================
void FSM_Update() {
    uint32_t now = HAL_GetMillis();

    // ---- Slow-loop: periodic sensor read (BMP280 pressure) ----
    if ((now - _lastSensorMs) >= SENSOR_POLL_INTERVAL_MS) {
        _lastSensorMs = now;

        // Read BMP280 differential pressure
        _currentPressureKpa = HAL_Sensors_ReadPressureKpa();

        // Read flow sensor (ADC-based)
        float rawADC = (float)HAL_Sensors_ReadFlowRaw();
        float deltaADC = rawADC - HAL_Sensors_GetFlowZero();
        float rawFlow = Filter_AdcToFlowLPM(deltaADC);
        _currentFlowLPM = Filter_EMA_Update(&_flowFilter, rawFlow);

        Safety_Update(_currentPressureKpa);
    }

    // ---- Telemetry print (during active ventilation only) ----
    if ((_state == STATE_INHALE || _state == STATE_HOLD ||
         _state == STATE_EXHALE || _state == STATE_PAUSE) &&
        (now - _lastTelemetryMs) >= TELEMETRY_PRINT_INTERVAL_MS) {
        _lastTelemetryMs = now;

        bool isExhalePhase = (_state == STATE_EXHALE || _state == STATE_PAUSE);
        float calcFlow = Kin_GetInstantaneousFlowLPM(isExhalePhase);

        float pressureCmH2O = _currentPressureKpa * 10.1972f;

        if (_graphMode) {
            // Serial Plotter format
            Serial.print(pressureCmH2O, 2);
            Serial.print('\t');
            Serial.print(_currentFlowLPM, 1);
            Serial.print('\t');
            Serial.print(calcFlow, 1);
            Serial.print('\t');
            Serial.println(Kin_GetStepsCompleted());
        } else {
            const char* phaseLabel;
            switch (_state) {
                case STATE_INHALE: phaseLabel = "INH"; break;
                case STATE_HOLD:   phaseLabel = "HLD"; break;
                case STATE_EXHALE: phaseLabel = "EXH"; break;
                case STATE_PAUSE:  phaseLabel = "PAU"; break;
                case STATE_SOFT_STOP_WAIT: phaseLabel = "S_W"; break;
                case STATE_RETRACT_HOME: phaseLabel = "S_R"; break;
                default:           phaseLabel = "???"; break;
            }
            Serial.print(F("  ")); Serial.print(phaseLabel);
            Serial.print(F(" Pressure=")); Serial.print(pressureCmH2O, 2);
            Serial.print(F("cmH2O  Flow=")); Serial.print(_currentFlowLPM, 1);
            Serial.print(F("L/min  CalcFlow=")); Serial.print(calcFlow, 1);
            Serial.print(F("L/min  Vol="));
            Serial.print(FSM_GetDeliveredVolumeMl(), 1);
            Serial.print(F("mL  Stp="));
            Serial.print(Kin_GetStepsCompleted());
            Serial.print(F("/")); Serial.println(_currentInhaleSteps > 0
                ? _currentInhaleSteps : Kin_GetStrokeSteps());
        }
    }

    // ---- Global fault override ----
    if (Safety_IsFaulted() && _state != STATE_FAULT) {
        Kin_Stop();
        _state        = STATE_FAULT;
        _stateEntryMs = now;
        Serial.println(F("[FSM] FAULT detected — motor disabled."));
        return;
    }

    // ---- Hardware Rocker Switch Poll ----
    static bool lastRockerState = true;
    bool currentRocker = (digitalRead(PIN_START_STOP) == LOW); // LOW = RUN
    if (currentRocker && !lastRockerState) {
        if (_state == STATE_READY || _state == STATE_SOFT_STOP_WAIT || _state == STATE_BOOT) {
            FSM_StartVentilation();
        }
    } else if (!currentRocker && lastRockerState) {
        FSM_RequestStopAtEnd();
    }
    lastRockerState = currentRocker;

    // ---- State machine ----
    switch (_state) {

    // ----------------------------------------------------------
    // BOOT: Wait for user to send 'H' to start homing
    // ----------------------------------------------------------
    case STATE_BOOT:
        Safety_SetLEDs(false, true, false);     // Yellow = waiting
        // Idle — FSM_StartCalibration() transitions to STATE_CALIBRATE
        break;

    // ----------------------------------------------------------
    // CALIBRATE: Retract slowly until Hall sensor triggers
    // ----------------------------------------------------------
    case STATE_CALIBRATE:
        if (!_calibRetracting) {
            // Phase A: retract slowly until Hall triggers
            if (HAL_Sensors_IsHallTriggered()) {
                Kin_Stop();
                _settings.maxCompressSteps = MECH_FULL_COMPRESS_STEPS;
                Serial.println(F("[CAL] Hall triggered — home found!"));
                Serial.print(F("[CAL] Max compress = "));
                Serial.print(_settings.maxCompressSteps);
                Serial.println(F(" steps"));

                Safety_SetLEDs(true, false, false);     // Green = ready
                _state        = STATE_READY;
                _stateEntryMs = now;
                HAL_Motor_Disable();

                // Recalculate kinematics with calibrated limits
                _updateKinematics();
                Serial.println(F("[FSM] System READY. Send 'S' to start ventilation."));
            }
            else if ((now - _stateEntryMs) > 30000UL) {
                Kin_Stop();
                HAL_Motor_Disable();
                Safety_SetFault(FAULT_HALL_NOT_FOUND);
                Serial.println(F("[CAL] FAULT: Hall sensor not found after 30s!"));
            }
            else {
                Kin_Update();   // Continue retracting slowly
            }
        }
        break;

    // ----------------------------------------------------------
    // READY: Waiting for ventilation start
    // ----------------------------------------------------------
    case STATE_READY:
        break;

    // ----------------------------------------------------------
    // INHALE: Motor compressing with S-Curve profile
    // ----------------------------------------------------------
    case STATE_INHALE: {
        Kin_Update();

        // Track peak pressure for Disconnect Alarm
        if (_currentPressureKpa > _peakPressureKpa) {
            _peakPressureKpa = _currentPressureKpa;
        }

        // PCV: stop advancing once target PIP is reached
        if (_mode == MODE_PCV) {
            float pressureError = _settings.targetPIP_kPa - _currentPressureKpa;
            
            // Proportional "Soft Landing" ramp
            if (pressureError < 0.5f && pressureError > 0.0f) { 
                float speedFactor = max(0.1f, pressureError / 0.5f); 
                Kin_SetCruiseInterval((uint32_t)(Kin_GetInhaleCruiseUs() / speedFactor)); 
            }
            
            // Safety Hard-Stop
            if (_currentPressureKpa >= _settings.targetPIP_kPa) {
                Kin_Stop();
                if (!_graphMode) {
                    Serial.print(F("  [PIP] Target reached at P="));
                    Serial.print(_currentPressureKpa, 2);
                    Serial.println(F(" kPa"));
                }
            }
        }

        // Transition: motor done OR inhale time exceeded
        uint32_t elapsed = now - _stateEntryMs;
        if (Kin_IsComplete() || elapsed >= _inhaleTimeMs) {
            _currentInhaleSteps = Kin_GetStepsCompleted();

            // Disconnect Alarm Check (Peak pressure < 5 cmH2O)
            if (_peakPressureKpa < 0.5f) {
                Safety_SetFault(FAULT_DISCONNECT);
                Serial.println(F("[FSM] FAULT: Patient Disconnected! (Peak PIP < 5 cmH2O)"));
            }

            if (!_graphMode) {
                Serial.print(F("\n  >> DELIVERED: "));
                Serial.print(_currentInhaleSteps / MECH_STEPS_PER_ML, 1);
                Serial.println(F(" mL <<"));
            }

            // Enter inspiratory hold for remaining inhale time
            int32_t holdMs = (int32_t)_inhaleTimeMs - (int32_t)elapsed;
            if (holdMs > 10) {
                _holdDurationMs = (uint32_t)holdMs;
                _state        = STATE_HOLD;
                _stateEntryMs = now;
                if (!_graphMode) {
                    Serial.print(F("  [HOLD] Plateau: "));
                    Serial.print(holdMs); Serial.println(F("ms"));
                }
            } else {
                // No time for hold — go directly to exhale
                _holdDurationMs = 0;
                _state = STATE_HOLD;
                _stateEntryMs = now;
            }
        }
        break;
    }

    // ----------------------------------------------------------
    // HOLD: Inspiratory plateau — motor stationary at peak
    // Wait for _holdDurationMs, then transition to EXHALE.
    // ----------------------------------------------------------
    case STATE_HOLD: {
        uint32_t holdElapsed = now - _stateEntryMs;
        if (holdElapsed >= _holdDurationMs) {
            // Begin exhale with S-Curve profile
            HAL_Motor_SetDirection(MOTOR_DIR_RETRACT);
            Kin_PlanExhale(_currentInhaleSteps);

            Safety_SetLEDs(false, true, false);     // Yellow = exhaling
            _state        = STATE_EXHALE;
            _stateEntryMs = now;

            if (!_graphMode) {
                Serial.println(F("--- EXHALE ---"));
                Serial.print(F("  Retracting "));
                Serial.print(_currentInhaleSteps);
                Serial.print(F(" steps  Cruise: "));
                Serial.print(Kin_GetExhaleCruiseUs());
                Serial.println(F("us"));
            }
        }
        break;
    }

    // ----------------------------------------------------------
    // EXHALE: Motor retracting with S-Curve profile
    // ----------------------------------------------------------
    case STATE_EXHALE: {
        Kin_Update();

        uint32_t elapsed = now - _stateEntryMs;
        if (Kin_IsComplete() || elapsed >= _exhaleTimeMs) {
            if (_stopRequested) {
                // Skip pause, retract slowly to home sensor and stop
                Serial.println(F("[FSM] Stop requested. Finding home sensor..."));
                HAL_Motor_Enable();
                HAL_Motor_SetDirection(MOTOR_DIR_RETRACT);
                Kin_PlanConstantMove(KIN_CALIBRATE_INTERVAL_US);
                _state = STATE_RETRACT_HOME;
                _stateEntryMs = now;
                _stopRequested = false;
            } else {
                // Enter expiratory pause for remaining exhale time
                int32_t pauseMs = (int32_t)_exhaleTimeMs - (int32_t)elapsed;
            if (pauseMs > 10) {
                _pauseDurationMs = (uint32_t)pauseMs;
                _state        = STATE_PAUSE;
                _stateEntryMs = now;
                if (!_graphMode) {
                    Serial.print(F("  [PAUSE] Expiratory: "));
                    Serial.print(pauseMs); Serial.println(F("ms"));
                }
            } else {
                // No time for pause — start next breath immediately
                if (!_graphMode) {
                    Serial.println(F("========== BREATH COMPLETE ==========\n"));
                }
                _startInhale();
            }
        }
        break;
    }

    // ----------------------------------------------------------
    // PAUSE: Expiratory pause — motor at home, waiting
    // Wait for _pauseDurationMs, then start next breath.
    // Patient-Triggered Assist-Control (A/C) Mode is active here.
    // ----------------------------------------------------------
    case STATE_PAUSE: {
        uint32_t pauseElapsed = now - _stateEntryMs;
        
        // --- Assist-Control (A/C) Trigger Logic ---
        // If the patient attempts to inhale, the pressure will drop.
        // We use a -0.2 kPa (-2.0 cmH2O) threshold.
        static uint8_t acTriggerCount = 0;
        if (_currentPressureKpa < -0.2f) {
            acTriggerCount++;
        } else {
            acTriggerCount = 0;
        }

        // Require 3 consecutive slow-loop polls (approx 120ms) to trigger a breath.
        // This acts as a debounce window to ignore random 40ms electrical spikes.
        bool patientTriggered = (acTriggerCount >= 3);

        if (pauseElapsed >= _pauseDurationMs || patientTriggered) {
            if (_stopRequested) {
                Serial.println(F("[FSM] Stop requested. Finding home sensor..."));
                HAL_Motor_Enable();
                HAL_Motor_SetDirection(MOTOR_DIR_RETRACT);
                Kin_PlanConstantMove(KIN_CALIBRATE_INTERVAL_US);
                _state = STATE_RETRACT_HOME;
                _stateEntryMs = now;
                _stopRequested = false;
            } else {
                if (!_graphMode) {
                    if (patientTriggered) {
                        Serial.println(F("========== PATIENT TRIGGERED BREATH ==========\n"));
                    } else {
                        Serial.println(F("========== BREATH COMPLETE ==========\n"));
                    }
                }
                acTriggerCount = 0; // reset
                _startInhale();
            }
        }
        break;
    }

    // ----------------------------------------------------------
    // SOFT STOP WAIT: Pausing 1.5s before retracting
    // ----------------------------------------------------------
    case STATE_SOFT_STOP_WAIT:
        if ((now - _stateEntryMs) >= 1500UL) {
            Serial.println(F("[FSM] Retracting slowly to home..."));
            HAL_Motor_Enable();
            HAL_Motor_SetDirection(MOTOR_DIR_RETRACT);
            Kin_PlanConstantMove(KIN_CALIBRATE_INTERVAL_US);
            _state = STATE_RETRACT_HOME;
            _stateEntryMs = now;
        }
        break;

    // ----------------------------------------------------------
    // RETRACT HOME: Slowly moving to home sensor
    // ----------------------------------------------------------
    case STATE_RETRACT_HOME:
        if (HAL_Sensors_IsHallTriggered()) {
            Kin_Stop();
            HAL_Motor_Disable();
            Safety_SetLEDs(true, false, false);
            _state = STATE_READY;
            _stateEntryMs = now;
            Serial.println(F("[FSM] Soft Stop Complete. System READY."));
        } else if ((now - _stateEntryMs) > 30000UL) {
            Kin_Stop();
            HAL_Motor_Disable();
            Safety_SetFault(FAULT_HALL_NOT_FOUND);
            Serial.println(F("[FSM] FAULT: Hall sensor not found during retract!"));
        } else {
            Kin_Update(); // Continue moving slowly
        }
        break;

    // ----------------------------------------------------------
    // FAULT: Motor disabled, alarm active
    // ----------------------------------------------------------
    case STATE_FAULT:
        HAL_Motor_Disable();
        break;
    }
}

// =============================================================
// GETTERS
// =============================================================
VentState FSM_GetState() { return _state; }
VentMode  FSM_GetMode()  { return _mode;  }

// =============================================================
// SETTERS — All recalculate kinematics immediately
// =============================================================
void FSM_SetMode(VentMode mode) { _mode = mode; }

void FSM_SetBPM(uint8_t bpm) {
    _settings.bpm = constrain(bpm, 10, 30);
    _updateKinematics();
}

void FSM_SetIERatio(float ratio) {
    _settings.ieRatio = ratio;
    _updateKinematics();
}

void FSM_SetTidalVolumeMl(float ml) {
    _settings.targetTidalVolume_mL = constrain(ml, 50.0f, MECH_MAX_TV_ML);
    _updateKinematics();
}

void FSM_SetTargetPIP(float kpa) {
    _settings.targetPIP_kPa = kpa;
}

// =============================================================
// START / STOP / CALIBRATE
// =============================================================
void FSM_StartVentilation() {
    if (_state != STATE_READY) {
        Serial.println(F("[ERR] Not ready. Home first (H)."));
        return;
    }
    Serial.println(F("[FSM] Ventilation STARTED."));
    _startInhale();
}

void FSM_SoftStopVentilation() {
    Kin_Stop();
    Safety_SetLEDs(false, true, false); // Yellow = pausing/waiting
    _state        = STATE_SOFT_STOP_WAIT;
    _stateEntryMs = HAL_GetMillis();
    Serial.println(F("[FSM] Soft Stop requested. Pausing 1.5s..."));
}

void FSM_RequestStopAtEnd() {
    _stopRequested = true;
    Serial.println(F("[FSM] Stop requested. Will halt after current breath cycle."));
}

void FSM_EmergencyStop() {
    Kin_Stop();
    HAL_Motor_Disable();
    _state        = STATE_BOOT;
    _stateEntryMs = HAL_GetMillis();
    Serial.println(F("[FSM] EMERGENCY STOPPED. Must Home (H) before starting again."));
}

void FSM_StartCalibration() {
    if (_state != STATE_BOOT && _state != STATE_READY) {
        Serial.println(F("[ERR] Can only calibrate from BOOT or READY."));
        return;
    }

    // Check if already at home
    if (HAL_Sensors_IsHallTriggered()) {
        _settings.maxCompressSteps = MECH_FULL_COMPRESS_STEPS;
        Serial.println(F("[CAL] Already at home! Hall triggered."));
        Safety_SetLEDs(true, false, false);
        _state        = STATE_READY;
        _stateEntryMs = HAL_GetMillis();
        _updateKinematics();
        Serial.println(F("[FSM] System READY. Send 'S' to start ventilation."));
        return;
    }

    Serial.println(F("[CAL] Retracting slowly to find Hall sensor..."));
    HAL_Motor_Enable();
    HAL_Motor_SetDirection(MOTOR_DIR_RETRACT);
    // Use slow constant speed for safe homing
    Kin_PlanConstantMove(KIN_CALIBRATE_INTERVAL_US);
    _calibRetracting = false;
    _state           = STATE_CALIBRATE;
    _stateEntryMs    = HAL_GetMillis();
}

// =============================================================
// GETTERS for telemetry
// =============================================================
const VentSettings* FSM_GetSettings()       { return &_settings; }
float     FSM_GetCurrentPressure()          { return _currentPressureKpa; }
float     FSM_GetCurrentFlowLPM()           { return _currentFlowLPM; }
uint32_t  FSM_GetInhaleTimeMs()             { return _inhaleTimeMs; }
uint32_t  FSM_GetExhaleTimeMs()             { return _exhaleTimeMs; }
void      FSM_SetGraphMode(bool enabled)    { _graphMode = enabled; }

float FSM_GetDeliveredVolumeMl() {
    float currentVol = (float)Kin_GetStepsCompleted() / MECH_STEPS_PER_ML;
    if (_state == STATE_EXHALE || _state == STATE_PAUSE || _state == STATE_RETRACT_HOME) {
        float maxVol = (float)_currentInhaleSteps / MECH_STEPS_PER_ML;
        float remaining = maxVol - currentVol;
        return (remaining > 0.0f) ? remaining : 0.0f;
    }
    return currentVol;
}
