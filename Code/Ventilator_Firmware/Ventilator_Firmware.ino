// ===========================================================
//  Ventilator_Firmware.ino
//  Smart E-Ventilator — Main Entry Point
//  Version 2.0 — 14 May 2026
//
//  Synchronized with validated Motor_Torque_Test.ino:
//    - S-Curve motion profiling (30/40/30)
//    - Dynamic kinematics (BPM, I:E, TV → motor speed/steps)
//    - Dual BMP280 differential pressure
//    - I2C timeout protection (Wire.setWireTimeout)
//    - ENA polarity fix (LOW = enabled)
//    - Step-based volume (not flow-integrated)
//    - Inspiratory hold + expiratory pause phases
//    - User-triggered homing calibration
//
//  Architecture
//  ┌─────────────────────────────────────────┐
//  │  APPLICATION  — FSM_App (State Machine) │
//  ├─────────────────────────────────────────┤
//  │  SERVICE      — Kinematics, Filters,    │
//  │                 Safety                  │
//  ├─────────────────────────────────────────┤
//  │  HAL          — HAL_Board, HAL_Motor,   │
//  │                 HAL_Sensors             │
//  └─────────────────────────────────────────┘
//
//  Board : Arduino UNO  (ATmega328P @ 16 MHz)
//  Driver: CS-D508      (800 pulses/rev, ENA+ active LOW)
//  Motor : CS-M22331    (NEMA 23, 5 A/phase)
// ===========================================================
#include "HAL_Board.h"
#include "HAL_Motor.h"
#include "HAL_Sensors.h"
#include "Kinematics.h"
#include "Filters.h"
#include "Safety.h"
#include "FSM_App.h"

// =============================================================
// LOCAL SETTING MIRRORS  (for serial display / adjustment)
// =============================================================
static uint8_t _bpm      = 15;
static float   _ieRatio  = 2.0f;
static float   _tvMl     = 400.0f;
static float   _pipKpa   = 2.5f;

// =============================================================
// HELPER: Read an integer from Serial input buffer
// =============================================================
static int32_t _readSerialInt() {
    int32_t val = 0;
    bool hasDigit = false;
    delay(30);   // Brief wait for remaining chars to arrive
    while (Serial.available()) {
        char c = Serial.peek();
        if (c >= '0' && c <= '9') {
            Serial.read();
            val = val * 10 + (c - '0');
            hasDigit = true;
        } else {
            break;
        }
    }
    return hasDigit ? val : -1;
}

// =============================================================
// HELPER: Print all current settings
// =============================================================
static void _printStatus() {
    const VentSettings* s = FSM_GetSettings();

    Serial.println(F("\n--- System Status ---"));

    Serial.print(F("  State : "));
    switch (FSM_GetState()) {
        case STATE_BOOT:      Serial.println(F("BOOT (waiting for H)"));   break;
        case STATE_CALIBRATE: Serial.println(F("CALIBRATING"));            break;
        case STATE_READY:     Serial.println(F("READY"));                  break;
        case STATE_INHALE:    Serial.println(F("INHALE"));                 break;
        case STATE_HOLD:      Serial.println(F("HOLD (plateau)"));         break;
        case STATE_EXHALE:    Serial.println(F("EXHALE"));                 break;
        case STATE_PAUSE:     Serial.println(F("PAUSE"));                  break;
        case STATE_FAULT:     Serial.println(F("FAULT"));                  break;
    }

    Serial.print(F("  Mode  : "));
    Serial.println(FSM_GetMode() == MODE_VCV ? F("VCV") : F("PCV"));

    Serial.print(F("  BPM   : ")); Serial.println(s->bpm);
    Serial.print(F("  I:E   : 1:")); Serial.println(s->ieRatio, 1);

    Serial.print(F("  TV    : ")); Serial.print(s->targetTidalVolume_mL, 1);
    Serial.print(F(" mL (")); Serial.print(Kin_GetStrokeSteps());
    Serial.print(F(" / ")); Serial.print(MECH_FULL_COMPRESS_STEPS);
    Serial.println(F(" steps)"));

    Serial.print(F("  PIP   : ")); Serial.print(s->targetPIP_kPa, 1);
    Serial.println(F(" kPa"));

    Serial.print(F("  T_inh : ")); Serial.print(FSM_GetInhaleTimeMs());
    Serial.println(F(" ms"));
    Serial.print(F("  T_exh : ")); Serial.print(FSM_GetExhaleTimeMs());
    Serial.println(F(" ms"));

    Serial.print(F("  Inh Cr: ")); Serial.print(Kin_GetInhaleCruiseUs());
    Serial.print(F("us (")); Serial.print(1000000UL / Kin_GetInhaleCruiseUs());
    Serial.println(F(" stp/s)"));

    Serial.print(F("  Exh Cr: ")); Serial.print(Kin_GetExhaleCruiseUs());
    Serial.print(F("us (")); Serial.print(1000000UL / Kin_GetExhaleCruiseUs());
    Serial.println(F(" stp/s)"));

    float rpm = (1000000.0f / Kin_GetInhaleCruiseUs() / MOTOR_PULSES_PER_REV) * 60.0f;
    Serial.print(F("  RPM   : ")); Serial.println(rpm, 1);

    Serial.print(F("  P_now : ")); Serial.print(FSM_GetCurrentPressure(), 2);
    Serial.print(F(" kPa  (")); Serial.print(FSM_GetCurrentPressure() * 10.1972f, 1);
    Serial.println(F(" cmH2O)"));

    Serial.print(F("  Flow  : ")); Serial.print(FSM_GetCurrentFlowLPM(), 1);
    Serial.println(F(" L/min"));

    Serial.print(F("  BMP280: "));
    Serial.println(HAL_Sensors_IsPressureOk() ? F("OK") : F("SENSOR ERROR"));

    Serial.println(F("------------------------\n"));
}

// =============================================================
// SERIAL COMMAND INTERFACE
//
// Single-char commands:
//   S / s  -- Start ventilation
//   X / x  -- Stop ventilation (emergency stop)
//   V / v  -- Switch to VCV
//   P / p  -- Switch to PCV
//   G / g  -- Toggle graph mode (Serial Plotter format)
//   H / h  -- Home / Calibrate (user-triggered)
//   ?      -- Print full status
//
// Parameterised commands (char + number):
//   B<nn>  — Set BPM        e.g. B20   → 20 BPM
//   R<nn>  — Set I:E ratio  e.g. R20   → 1:2.0
//   T<nnn> — Set tidal vol  e.g. T400  → 400 mL
//   I<nn>  — Set PIP        e.g. I25   → 2.5 kPa
//
// Simple adjust:
//   +  —  BPM + 1
//   -  —  BPM - 1
// =============================================================
static void _processSerialCommand() {
    if (!Serial.available()) return;

    char cmd = Serial.read();
    switch (cmd) {
        // --- Start / Stop ---
        case 'S': case 's':
            FSM_StartVentilation();
            break;
        case 'X': case 'x':
            FSM_SoftStopVentilation();
            break;
        case 'E': case 'e':
            FSM_EmergencyStop();
            break;

        // --- Homing (user-triggered) ---
        case 'H': case 'h':
            FSM_StartCalibration();
            break;

        // --- Reboot ---
        case 'R': case 'r':
            Serial.println(F("[CMD] Rebooting..."));
            HAL_WDT_ForceReboot();
            break;

        // --- Mode ---
        case 'V': case 'v':
            FSM_SetMode(MODE_VCV);
            Serial.println(F("[CMD] Mode -> VCV"));
            break;
        case 'P': case 'p':
            FSM_SetMode(MODE_PCV);
            Serial.println(F("[CMD] Mode -> PCV"));
            break;

        // --- Graph mode toggle ---
        case 'G': case 'g': {
            static bool graphOn = false;
            graphOn = !graphOn;
            FSM_SetGraphMode(graphOn);
            Serial.println(graphOn ? F("[CMD] Graph mode ON  (use Serial Plotter)") :
                                     F("[CMD] Graph mode OFF (text telemetry)"));
            break;
        }

        // --- BPM quick adjust ---
        case '+':
            _bpm = constrain(_bpm + 1, 10, 30);
            FSM_SetBPM(_bpm);
            Serial.print(F("[CMD] BPM = ")); Serial.println(_bpm);
            break;
        case '-':
            _bpm = constrain(_bpm - 1, 10, 30);
            FSM_SetBPM(_bpm);
            Serial.print(F("[CMD] BPM = ")); Serial.println(_bpm);
            break;

        // --- BPM set exact ---
        case 'B': case 'b': {
            int32_t val = _readSerialInt();
            if (val >= 10 && val <= 30) {
                _bpm = (uint8_t)val;
                FSM_SetBPM(_bpm);
                Serial.print(F("[SET] BPM = ")); Serial.println(_bpm);
            } else if (val == -1) {
                Serial.println(F("[ERR] Usage: B20 (BPM 10-30)"));
            } else {
                Serial.println(F("[ERR] BPM must be 10-30."));
            }
            break;
        }

        // --- I:E Ratio ---
        case 'R': case 'r': {
            int32_t val = _readSerialInt();
            if (val >= 10 && val <= 40) {
                _ieRatio = val / 10.0f;
                FSM_SetIERatio(_ieRatio);
                Serial.print(F("[SET] I:E = 1:")); Serial.println(_ieRatio, 1);
            } else {
                Serial.println(F("[ERR] I:E range: R10-R40 (1:1.0 to 1:4.0)"));
            }
            break;
        }

        // --- Tidal Volume (mL) ---
        case 'T': case 't': {
            int32_t val = _readSerialInt();
            if (val >= 50 && val <= (int32_t)MECH_MAX_TV_ML) {
                _tvMl = (float)val;
                FSM_SetTidalVolumeMl(_tvMl);
                Serial.print(F("[SET] TV = ")); Serial.print(_tvMl, 0);
                Serial.print(F(" mL (")); Serial.print(Kin_GetStrokeSteps());
                Serial.println(F(" steps)"));
            } else {
                Serial.print(F("[ERR] TV range: 50-"));
                Serial.print((int)MECH_MAX_TV_ML);
                Serial.println(F(" mL"));
            }
            break;
        }

        // --- Target PIP (kPa × 10) ---
        case 'I': case 'i': {
            int32_t val = _readSerialInt();
            if (val >= 5 && val <= 50) {
                _pipKpa = val / 10.0f;
                FSM_SetTargetPIP(_pipKpa);
                Serial.print(F("[SET] PIP = ")); Serial.print(_pipKpa, 1);
                Serial.println(F(" kPa"));
            } else {
                Serial.println(F("[ERR] PIP range: I5-I50 (0.5-5.0 kPa)"));
            }
            break;
        }

        // --- Status ---
        case '?':
            _printStatus();
            break;

        case '\n': case '\r': break;
        default: break;
    }
}

// =============================================================
// SETUP
// =============================================================
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);

    Serial.println(F("\n=========================================="));
    Serial.println(F("  Smart E-Ventilator Firmware v2.0"));
    Serial.println(F("  HAL + Service + FSM Architecture"));
    Serial.println(F("==========================================\n"));

    // HAL Layer
    HAL_Board_Init();       // Pins, I2C bus with timeout
    HAL_Motor_Init();       // Motor disabled at boot
    HAL_Sensors_Init();     // ADC settle

    // BMP280 Pressure Sensors (I2C)
    HAL_Sensors_InitPressure();

    // Flow Sensor Auto-Zero (3s warm-up)
    HAL_Sensors_AutoZeroFlow();

    // Service Layer
    Kin_Init();
    Safety_Init();

    // Application Layer
    FSM_Init();

    // Enable Watchdog Timer (500ms timeout — accommodates I2C delays)
    HAL_WDT_Enable();

    Serial.println(F("\n[BOOT] All modules initialised."));
    Serial.println(F("Commands:"));
    Serial.println(F("  H=Home  S=Start  X=Stop  V=VCV  P=PCV  G=Graph  ?=Status"));
    Serial.println(F("  B<nn>=BPM  R<nn>=I:E  T<nnn>=TidalVol(mL)  I<nn>=PIP"));
    Serial.println(F("  + / - = BPM up/down\n"));
}

// =============================================================
// LOOP
// =============================================================
void loop() {
    HAL_WDT_Reset();
    FSM_Update();
    _processSerialCommand();
}
