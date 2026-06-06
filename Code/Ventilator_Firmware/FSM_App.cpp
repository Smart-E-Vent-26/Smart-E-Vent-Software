// ===========================================================
// FSM_App.cpp — Application Layer: Ventilator State Machine
// Smart E-Ventilator Firmware v2.0
// ===========================================================
#include "FSM_App.h"
#include "HAL_Board.h"
#include "HAL_Motor.h"
#include "HAL_Sensors.h"
#include "Kinematics.h"
#include "Filters.h"
#include "Safety.h"
#include <avr/wdt.h>

#define DEFAULT_BPM                 15
#define DEFAULT_IE_RATIO            2.0f      // 1:2
#define DEFAULT_TIDAL_ML            400.0f    // 400 mL
#define DEFAULT_TARGET_PIP_KPA      2.5f      // ~25 cmH2O
#define SENSOR_POLL_INTERVAL_MS     40        // ~25 Hz
#define TELEMETRY_PRINT_INTERVAL_MS 250       // Print to Serial every 250ms

static VentState    _state            = STATE_BOOT;
static VentMode     _mode             = MODE_VCV;
static VentSettings _settings;
static EMA_Filter   _flowFilter;
static bool         _stopRequested    = false;

static uint32_t _stateEntryMs    = 0;
static uint32_t _lastSensorMs    = 0;
static uint32_t _lastTelemetryMs = 0;

static uint32_t _inhaleTimeMs    = 0;
static uint32_t _exhaleTimeMs    = 0;

static int32_t  _currentInhaleSteps = 0;
static float    _currentPressureKpa = 0.0f;
static float    _peakPressureKpa    = 0.0f;
static float    _currentFlowLPM     = 0.0f;
static uint32_t _holdDurationMs     = 0;
static uint32_t _pauseDurationMs    = 0;

static bool     _graphMode          = false;
static bool     _calibRetracting    = false;

static void _startInhale();

static void _computeBreathTiming() {
    uint32_t breathPeriodMs = 60000UL / _settings.bpm;
    float    totalRatio     = 1.0f + _settings.ieRatio;
    _inhaleTimeMs = (uint32_t)(breathPeriodMs / totalRatio);
    _exhaleTimeMs = breathPeriodMs - _inhaleTimeMs;
}

static void _updateKinematics() {
    Kin_UpdateDynamics(_settings.bpm, _settings.ieRatio, _settings.targetTidalVolume_mL);
    _computeBreathTiming();
}

static void _finishBreath(bool patientTriggered = false) {
    if (_stopRequested) {
        _stopRequested = false;
        
        if (HAL_Sensors_IsHallTriggered()) {
            if (!_graphMode) Serial.println(F("========== VENTILATION STOPPING: ALREADY AT HOME ==========\n"));
            HAL_Motor_Disable();
            Safety_SetLEDs(true, false, false); // Green
            _state = STATE_READY;
            _stateEntryMs = HAL_GetMillis();
        } else {
            if (!_graphMode) Serial.println(F("========== VENTILATION STOPPING: RETRACTING TO HOME ==========\n"));
            _state        = STATE_SOFT_STOP_WAIT;
            _stateEntryMs = HAL_GetMillis();
        }
    } else {
        if (!_graphMode) {
            if (patientTriggered)
                Serial.println(F("========== PATIENT TRIGGERED BREATH ==========\n"));
            else
                Serial.println(F("========== BREATH COMPLETE ==========\n"));
        }
        _startInhale();
    }
}

static void _startInhale() {
    _stateEntryMs    = HAL_GetMillis();
    _peakPressureKpa = 0.0f;

    _updateKinematics();

    int32_t targetSteps;
    if (_mode == MODE_VCV) {
        targetSteps = Kin_GetStrokeSteps();
        if (targetSteps > _settings.maxCompressSteps)
            targetSteps = _settings.maxCompressSteps;
    } else {
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
        Serial.print(F(" mL ("));
        Serial.print(targetSteps);
        Serial.print(F(" steps)  Cruise: "));
        Serial.print(Kin_GetInhaleCruiseUs());
        Serial.println(F("us"));
    }
}

void FSM_Init() {
    _state = STATE_BOOT;
    _mode  = MODE_VCV;

    _settings.bpm                  = DEFAULT_BPM;
    _settings.ieRatio              = DEFAULT_IE_RATIO;
    _settings.targetTidalVolume_mL = DEFAULT_TIDAL_ML;
    _settings.targetPIP_kPa        = DEFAULT_TARGET_PIP_KPA;
    _settings.maxCompressSteps     = MECH_FULL_COMPRESS_STEPS;

    Filter_EMA_Init(&_flowFilter, 0.15f);
    _updateKinematics();
    _stateEntryMs    = HAL_GetMillis();
    _calibRetracting = false;
    _stopRequested   = false;

    Serial.println(F("[FSM] Waiting for homing command (H)..."));
}

void FSM_Update() {
    uint32_t now = HAL_GetMillis();

    // ---- Hardware Emergency Stop (NC Wiring & Auto-Reboot) ----
    static bool lastEStopPressed = false;
    static uint32_t eStopActiveMs = 0;
    bool rawEStop = HAL_Board_ReadEStopBtn();

    if (rawEStop) {
        if (eStopActiveMs == 0) eStopActiveMs = now;
    } else {
        eStopActiveMs = 0;
    }

    bool currentEStopPressed = (eStopActiveMs != 0 && (now - eStopActiveMs > 50));

    if (currentEStopPressed) {
        if (_state != STATE_FAULT) {
            Kin_Stop();
            HAL_Motor_Disable();
            Safety_SetFault(FAULT_ESTOP);
            _state        = STATE_FAULT;
            _stateEntryMs = now;
            Serial.println(F("[FSM] HARDWARE E-STOP PRESSED! System Halted."));
        }
        lastEStopPressed = true;
    } else {
        if (lastEStopPressed) {
            Serial.println(F("[FSM] E-Stop Released. Rebooting system..."));
            delay(100);
            wdt_enable(WDTO_15MS);
            while (1) {}
        }
        lastEStopPressed = false;
    }

    // ---- Hardware Start/Stop (ROCKER SWITCH LOGIC) ----
    static bool     lastRockerState    = false;
    static uint32_t lastRockerChangeMs = 0;
    bool currentRockerState = HAL_Board_ReadStartStopBtn();

    if (currentRockerState != lastRockerState && (now - lastRockerChangeMs > 250)) {
        lastRockerChangeMs = now;

        if (currentRockerState == true) { 
            if (_state == STATE_READY) {
                FSM_StartVentilation();
            }
        } else { 
            if (_state == STATE_INHALE || _state == STATE_HOLD ||
                _state == STATE_EXHALE || _state == STATE_PAUSE) {
                _stopRequested = true;
                if (!_graphMode)
                    Serial.println(F("[FSM] Rocker OFF — finishing current phase before homing..."));
            }
        }
        lastRockerState = currentRockerState;
    }

    // ---- Slow-loop: periodic sensor read ----
    if ((now - _lastSensorMs) >= SENSOR_POLL_INTERVAL_MS) {
        _lastSensorMs = now;

        _currentPressureKpa = HAL_Sensors_ReadPressureKpa();

        float rawADC  = (float)HAL_Sensors_ReadFlowRaw();
        float deltaADC = rawADC - HAL_Sensors_GetFlowZero();
        float rawFlow  = Filter_AdcToFlowLPM(deltaADC);
        _currentFlowLPM = Filter_EMA_Update(&_flowFilter, rawFlow);

        Safety_Update(_currentPressureKpa);
    }

    // ---- Telemetry print (MATCHES OLD FILE EXACTLY) ----
    if ((_state == STATE_INHALE || _state == STATE_HOLD ||
         _state == STATE_EXHALE || _state == STATE_PAUSE) &&
        (now - _lastTelemetryMs) >= TELEMETRY_PRINT_INTERVAL_MS) {
        _lastTelemetryMs = now;

        bool  isExhalePhase   = (_state == STATE_EXHALE || _state == STATE_PAUSE);
        float calcFlow        = Kin_GetInstantaneousFlowLPM(isExhalePhase);
        float pressureCmH2O   = _currentPressureKpa * 10.1972f;

        if (_graphMode) {
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
                case STATE_INHALE:         phaseLabel = "INH"; break;
                case STATE_HOLD:           phaseLabel = "HLD"; break;
                case STATE_EXHALE:         phaseLabel = "EXH"; break;
                case STATE_PAUSE:          phaseLabel = "PAU"; break;
                default:                   phaseLabel = "???"; break;
            }
            Serial.print(F("  "));           Serial.print(phaseLabel);
            Serial.print(F(" Pressure="));   Serial.print(pressureCmH2O, 2);
            Serial.print(F("cmH2O  Flow=")); Serial.print(_currentFlowLPM, 1);
            Serial.print(F("L/min  CalcFlow=")); Serial.print(calcFlow, 1);
            Serial.print(F("L/min  Vol="));
            Serial.print(FSM_GetDeliveredVolumeMl(), 1);
            Serial.print(F("mL  Stp="));
            Serial.print(Kin_GetStepsCompleted());
            Serial.print(F("/"));
            Serial.println(_currentInhaleSteps > 0 ? _currentInhaleSteps : Kin_GetStrokeSteps());
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

    // ---- State machine ----
    switch (_state) {

    case STATE_BOOT:
        Safety_SetLEDs(false, true, false);
        break;

    case STATE_CALIBRATE:
        if (!_calibRetracting) {
            if (HAL_Sensors_IsHallTriggered()) {
                Kin_Stop();
                _settings.maxCompressSteps = MECH_FULL_COMPRESS_STEPS;
                Serial.println(F("[CAL] Hall triggered — home found!"));
                Safety_SetLEDs(true, false, false);
                _state        = STATE_READY;
                _stateEntryMs = now;
                HAL_Motor_Disable();
                _updateKinematics();
                Serial.println(F("[FSM] System READY. Send 'S' to start ventilation."));
            } else if ((now - _stateEntryMs) > 30000UL) {
                Kin_Stop();
                HAL_Motor_Disable();
                Safety_SetFault(FAULT_HALL_NOT_FOUND);
                Serial.println(F("[CAL] FAULT: Hall sensor not found after 30s!"));
            } else {
                Kin_Update();
            }
        }
        break;

    case STATE_READY:
        break;

    case STATE_INHALE: {
        Kin_Update();

        if (_currentPressureKpa > _peakPressureKpa) {
            _peakPressureKpa = _currentPressureKpa;
        }

        if (_mode == MODE_PCV) {
            float pressureError = _settings.targetPIP_kPa - _currentPressureKpa;
            if (pressureError < 0.5f && pressureError > 0.0f) {
                float speedFactor = max(0.1f, pressureError / 0.5f);
                Kin_SetCruiseInterval((uint32_t)(Kin_GetInhaleCruiseUs() / speedFactor));
            }
            if (_currentPressureKpa >= _settings.targetPIP_kPa) {
                Kin_Stop();
            }
        }

        uint32_t elapsed = now - _stateEntryMs;
        if (Kin_IsComplete() || elapsed >= _inhaleTimeMs) {
            _currentInhaleSteps = Kin_GetStepsCompleted();

            // PATIENT DISCONNECT ALARM RESTORED (Matches old file)
            if (_peakPressureKpa < 0.5f) {
                Safety_SetFault(FAULT_DISCONNECT);
                Serial.println(F("[FSM] FAULT: Patient Disconnected! (Peak PIP < 5 cmH2O)"));
            }

            int32_t holdMs = (int32_t)_inhaleTimeMs - (int32_t)elapsed;
            _holdDurationMs = (holdMs > 10) ? (uint32_t)holdMs : 0;
            _state        = STATE_HOLD;
            _stateEntryMs = now;
        }
        break;
    }

    case STATE_HOLD: {
        uint32_t holdElapsed = now - _stateEntryMs;
        if (holdElapsed >= _holdDurationMs) {
            HAL_Motor_SetDirection(MOTOR_DIR_RETRACT);
            Kin_PlanExhale(_currentInhaleSteps);
            Safety_SetLEDs(false, true, false);
            _state        = STATE_EXHALE;
            _stateEntryMs = now;
        }
        break;
    }

    case STATE_EXHALE: {
        Kin_Update();
        uint32_t elapsed = now - _stateEntryMs;

        // --- INSTANT HOMING TRIGGER ---
        if (_stopRequested && HAL_Sensors_IsHallTriggered()) {
            Kin_Stop();
            HAL_Motor_Disable();
            Safety_SetLEDs(true, false, false); // Green = Ready
            _state = STATE_READY;
            _stateEntryMs = now;
            _stopRequested = false;
            if (!_graphMode) Serial.println(F("[FSM] Hall sensor hit during exhale! System STOPPED and READY."));
            break; 
        }

        if (Kin_IsComplete() || elapsed >= _exhaleTimeMs) {
            int32_t pauseMs = (int32_t)_exhaleTimeMs - (int32_t)elapsed;
            if (_stopRequested) {
                _finishBreath();
            } else if (pauseMs > 10) {
                _pauseDurationMs = (uint32_t)pauseMs;
                _state        = STATE_PAUSE;
                _stateEntryMs = now;
            } else {
                _finishBreath();
            }
        }
        break;
    }

    case STATE_PAUSE: {
        uint32_t pauseElapsed = now - _stateEntryMs;
        static uint8_t acTriggerCount = 0;

        if (_stopRequested) {
            acTriggerCount = 0;
            _finishBreath();
            break;
        }

        if (_currentPressureKpa < -0.2f) {
            acTriggerCount++;
        } else {
            acTriggerCount = 0;
        }

        bool patientTriggered = (acTriggerCount >= 3);
        if (pauseElapsed >= _pauseDurationMs || patientTriggered) {
            acTriggerCount = 0;
            _finishBreath(patientTriggered);
        }
        break;
    }

    case STATE_SOFT_STOP_WAIT:
        if ((now - _stateEntryMs) >= 1500UL) {
            HAL_Motor_Enable();
            HAL_Motor_SetDirection(MOTOR_DIR_RETRACT);
            Kin_PlanConstantMove(KIN_CALIBRATE_INTERVAL_US);
            _state        = STATE_RETRACT_HOME;
            _stateEntryMs = now;
        }
        break;

    case STATE_RETRACT_HOME:
        if (HAL_Sensors_IsHallTriggered()) {
            Kin_Stop();
            HAL_Motor_Disable();
            Safety_SetLEDs(true, false, false);
            _state        = STATE_READY;
            _stateEntryMs = now;
            Serial.println(F("[FSM] Soft Stop Complete. System READY."));
        } else if ((now - _stateEntryMs) > 30000UL) {
            Kin_Stop();
            HAL_Motor_Disable();
            Safety_SetFault(FAULT_HALL_NOT_FOUND);
        } else {
            Kin_Update();
        }
        break;

    case STATE_FAULT:
        HAL_Motor_Disable();
        break;
    }
}

// =============================================================
// GETTERS & SETTERS
// =============================================================
VentState FSM_GetState()  { return _state; }
VentMode  FSM_GetMode()   { return _mode;  }

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

void FSM_StartVentilation() {
    if (_state != STATE_READY) {
        Serial.println(F("[ERR] Not ready. Home first (H)."));
        return;
    }
    _stopRequested = false;
    Serial.println(F("[FSM] Ventilation STARTED."));
    _startInhale();
}

void FSM_SoftStopVentilation() {
    if (_state == STATE_READY || _state == STATE_BOOT) return;
    _stopRequested = true;
    Serial.println(F("[FSM] Soft Stop requested (GUI). Waiting for breath to finish..."));
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
    Kin_PlanConstantMove(KIN_CALIBRATE_INTERVAL_US);
    _calibRetracting = false;
    _state           = STATE_CALIBRATE;
    _stateEntryMs    = HAL_GetMillis();
}

const VentSettings* FSM_GetSettings()    { return &_settings; }
float     FSM_GetCurrentPressure()       { return _currentPressureKpa; }
float     FSM_GetCurrentFlowLPM()        { return _currentFlowLPM; }
uint32_t  FSM_GetInhaleTimeMs()          { return _inhaleTimeMs; }
uint32_t  FSM_GetExhaleTimeMs()          { return _exhaleTimeMs; }
void      FSM_SetGraphMode(bool enabled) { _graphMode = enabled; }

float FSM_GetDeliveredVolumeMl() {
    float currentVol = (float)Kin_GetStepsCompleted() / MECH_STEPS_PER_ML;
    if (_state == STATE_EXHALE || _state == STATE_PAUSE ||
        _state == STATE_RETRACT_HOME) {
        float maxVol   = (float)_currentInhaleSteps / MECH_STEPS_PER_ML;
        float remaining = maxVol - currentVol;
        return (remaining > 0.0f) ? remaining : 0.0f;
    }
    return currentVol;
}