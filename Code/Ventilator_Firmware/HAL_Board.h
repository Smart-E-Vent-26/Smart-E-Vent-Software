// ===========================================================
// HAL_Board.h — Hardware Abstraction Layer: Board & Pin Defs
// Smart E-Ventilator Firmware v2.0
//
// FIX v2.0:
//   - Added missing declarations for HAL_Board_ReadStartStopBtn()
//     and HAL_Board_ReadEStopBtn() (were wrongly placed in HAL_Motor.h)
// ===========================================================
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <Arduino.h>

// =============================================================
// PIN DEFINITIONS
// Source: Electrical-System-Specification.md Rev 3.0
// =============================================================

// --- Motor Driver (CS-D508) ---
#define PIN_MOTOR_PUL       2     // Step Pulse  → PUL+
#define PIN_MOTOR_DIR       3     // Direction   → DIR+
#define PIN_MOTOR_ENA       4     // Enable      → ENA+ (active LOW)
#define PIN_MOTOR_ALM       9     // Alarm       ← ALM+ (INPUT_PULLUP)

// --- User Interface ---
#define PIN_BUZZER          5     // SFM-20B Piezo
#define PIN_LED_GREEN       6
#define PIN_LED_YELLOW      7
#define PIN_LED_RED         8
#define PIN_BTN_START_STOP  10    // Physical Start/Stop Toggle
#define PIN_BTN_ESTOP       A1    // Physical Emergency Stop

// --- Sensors ---
#define PIN_FLOW_SENSOR     A0    // MPX5010DP Differential Pressure
#define PIN_HALL_SENSOR     A2    // A3144 Hall Effect

// =============================================================
// MOTOR DRIVER CONFIGURATION
// =============================================================
#define MOTOR_PULSES_PER_REV    800   // CS-D508 DIP: OFF,ON,ON,ON

// =============================================================
// MECHANICAL CALIBRATION (validated on hardware)
//
// Calibrated: 1300 steps at full compression delivers 600 mL.
// Update MECH_FULL_COMPRESS_STEPS when mechanism changes.
// =============================================================
#define MECH_FULL_COMPRESS_STEPS    1300
#define MECH_FULL_COMPRESS_TURNS    (MECH_FULL_COMPRESS_STEPS / (float)MOTOR_PULSES_PER_REV)

// Volume ↔ Steps calibration (1300 steps = 600 mL)
const float MECH_STEPS_PER_ML           = 2.166667f;
const float MECH_MAX_TV_ML              = 600.0f;
const float MECH_INHALE_MOTION_FRACTION = 0.8f;   // 80% compression, 20% hold

// =============================================================
// HAL TIME WRAPPERS
// =============================================================
uint32_t HAL_GetMillis();
uint32_t HAL_GetMicros();

// =============================================================
// BOARD INIT & WATCHDOG
// =============================================================
void HAL_Board_Init();
void HAL_WDT_Enable();
void HAL_WDT_Reset();
void HAL_WDT_Disable();

// =============================================================
// BUTTON READS
// (Definitions live in HAL_Board.cpp)
// =============================================================
bool HAL_Board_ReadStartStopBtn();
bool HAL_Board_ReadEStopBtn();

#endif // HAL_BOARD_H
