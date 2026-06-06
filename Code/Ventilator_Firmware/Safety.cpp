// ===========================================================
// Safety.cpp — Service Layer: Fault Detection & Alarm Outputs
// Smart E-Ventilator Firmware v1.0
// ===========================================================
#include "Safety.h"
#include "HAL_Board.h"
#include "HAL_Motor.h"

// =============================================================
// PRIVATE STATE
// =============================================================
static uint8_t _faultCode = FAULT_NONE;

// =============================================================
// INIT
// =============================================================
void Safety_Init() {
    _faultCode = FAULT_NONE;
    Safety_SetLEDs(false, false, false);
    Safety_BuzzerOff();
}

// =============================================================
// Safety_Update — Polled every sensor cycle (~3 ms)
// Checks hardware alarm line and airway pressure limits.
// =============================================================
void Safety_Update(float currentPressureKpa) {
    // 1. Check motor driver ALM+ line with debounce (EMI protection)
    static uint8_t driverAlarmCount = 0;
    if (HAL_Motor_ReadAlarm()) {
        driverAlarmCount++;
        if (driverAlarmCount >= 5) {  // ~200ms of continuous alarm signal
            _faultCode |= FAULT_DRIVER_ALARM;
        }
    } else {
        driverAlarmCount = 0;
    }

    // 2. Check airway overpressure with debounce
    static uint8_t pressureAlarmCount = 0;
    if (currentPressureKpa > SAFETY_MAX_PIP_KPA) {
        pressureAlarmCount++;
        if (pressureAlarmCount >= 3) { // ~120ms of continuous overpressure
            _faultCode |= FAULT_OVERPRESSURE;
        }
    } else {
        pressureAlarmCount = 0;
    }

    // 3. Check Emergency Stop Switch (NC to A1)
    static uint8_t estopCount = 0;
    if (digitalRead(PIN_EMERGENCY_STOP) == HIGH) { // Open circuit = pressed
        estopCount++;
        if (estopCount >= 3) {
            _faultCode |= FAULT_EMERGENCY_STOP;
        }
    } else {
        estopCount = 0;
        if (_faultCode & FAULT_EMERGENCY_STOP) {
            // "reset it as reboot if I made it back again"
            HAL_WDT_ForceReboot();
        }
    }

    // 4. If ANY fault is active → hard-stop motor, alarm outputs
    static uint32_t faultStartTime = 0;
    if (_faultCode != FAULT_NONE) {
        HAL_Motor_Disable();
        Safety_SetLEDs(false, false, true);         // Red only
        
        if (faultStartTime == 0) faultStartTime = HAL_GetMillis();
        
        // Auto-mute the loud buzzer after 4 seconds to avoid sensory fatigue
        if (HAL_GetMillis() - faultStartTime < 4000 || (_faultCode & FAULT_EMERGENCY_STOP)) {
            if (_faultCode & FAULT_EMERGENCY_STOP) {
                Safety_BuzzerTone(ALARM_TONE_CRITICAL); // Continuous critical alarm
            } else if (_faultCode & FAULT_OVERPRESSURE) {
                Safety_BuzzerTone(ALARM_TONE_CRITICAL); // 1000 Hz
            } else if (_faultCode & FAULT_DISCONNECT) {
                Safety_BuzzerTone(ALARM_TONE_DISCONNECT); // 1500 Hz
            } else {
                Safety_BuzzerTone(ALARM_TONE_WARNING); // 2000 Hz
            }
        } else {
            Safety_BuzzerOff();
        }
    } else {
        faultStartTime = 0; // reset auto-mute timer
    }
}

// =============================================================
// Query / Mutate Faults
// =============================================================
bool    Safety_IsFaulted()            { return (_faultCode != FAULT_NONE); }
uint8_t Safety_GetFaultCode()         { return _faultCode; }

void Safety_SetFault(uint8_t code)    { _faultCode |= code; }

void Safety_ClearFault() {
    _faultCode = FAULT_NONE;
    Safety_BuzzerOff();
    Safety_SetLEDs(false, false, false);
}

// =============================================================
// LED Control — wraps HAL digital writes
// =============================================================
void Safety_SetLEDs(bool green, bool yellow, bool red) {
    digitalWrite(PIN_LED_GREEN,  green  ? HIGH : LOW);
    digitalWrite(PIN_LED_YELLOW, yellow ? HIGH : LOW);
    digitalWrite(PIN_LED_RED,    red    ? HIGH : LOW);
}

// =============================================================
// Buzzer Control
// =============================================================
void Safety_BuzzerTone(uint16_t freqHz) { tone(PIN_BUZZER, freqHz); }
void Safety_BuzzerOff()                 { noTone(PIN_BUZZER); }
