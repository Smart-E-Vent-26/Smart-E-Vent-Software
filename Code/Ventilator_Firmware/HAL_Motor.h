// ===========================================================
// HAL_Motor.h — Hardware Abstraction Layer: CS-D508 Driver
// Smart E-Ventilator Firmware v1.0
// ===========================================================
#ifndef HAL_MOTOR_H
#define HAL_MOTOR_H

#include <Arduino.h>

// Direction constants (matched to mechanical assembly)
#define MOTOR_DIR_COMPRESS   LOW    // Toward Ambu bag (forward)
#define MOTOR_DIR_RETRACT    HIGH   // Away from Ambu bag (backward)
// --- User Interface ---
#define PIN_BUZZER          5     // SFM-20B Piezo
#define PIN_LED_GREEN       6
#define PIN_LED_YELLOW      7
#define PIN_LED_RED         8
#define PIN_BTN_START_STOP  10    // Physical Start/Stop Toggle
#define PIN_BTN_ESTOP       A1    // Physical Emergency Stop

void    HAL_Motor_Init();
void    HAL_Motor_Enable();
void    HAL_Motor_Disable();
void    HAL_Motor_SetDirection(uint8_t dir);
void    HAL_Motor_StepPulse();       // Fire exactly one step pulse
bool    HAL_Motor_ReadAlarm();       // true = driver fault detected
bool HAL_Board_ReadStartStopBtn();
bool HAL_Board_ReadEStopBtn();
#endif // HAL_MOTOR_H
