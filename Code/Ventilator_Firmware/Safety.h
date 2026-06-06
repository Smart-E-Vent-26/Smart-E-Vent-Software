// ===========================================================
// Safety.h — Service Layer: Fault Detection & Alarm Outputs
// Smart E-Ventilator Firmware v1.0
// ===========================================================
#ifndef SAFETY_H
#define SAFETY_H

#include <Arduino.h>

// =============================================================
// FAULT CODES (bitmask — multiple faults can coexist)
// =============================================================
#define FAULT_NONE              0x00
#define FAULT_DRIVER_ALARM      0x01
#define FAULT_OVERPRESSURE      0x02
#define FAULT_SENSOR_FAIL       0x04
#define FAULT_HALL_NOT_FOUND    0x08
#define FAULT_DISCONNECT        0x10
#define FAULT_EMERGENCY_STOP    0x20

// =============================================================
// PRESSURE SAFETY LIMITS
// =============================================================
#define SAFETY_MAX_PIP_KPA      4.0f    // ~40 cmH2O — absolute ceiling
#define SAFETY_MIN_PEEP_KPA     0.0f    // Minimum PEEP baseline

// =============================================================
// ALARM TONES (Hz)
// =============================================================
#define ALARM_TONE_CRITICAL     1000
#define ALARM_TONE_WARNING      2000
#define ALARM_TONE_DISCONNECT   1500

// =============================================================
// PUBLIC API
// =============================================================
void    Safety_Init();
void    Safety_Update(float currentPressureKpa);   // Call every slow-loop tick
bool    Safety_IsFaulted();
uint8_t Safety_GetFaultCode();
void    Safety_SetFault(uint8_t code);              // Set a fault externally
void    Safety_ClearFault();

// LED helpers
void    Safety_SetLEDs(bool green, bool yellow, bool red);

// Buzzer helpers
void    Safety_BuzzerTone(uint16_t freqHz);
void    Safety_BuzzerOff();

#endif // SAFETY_H
