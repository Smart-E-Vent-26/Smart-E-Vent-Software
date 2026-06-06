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

void    HAL_Motor_Init();
void    HAL_Motor_Enable();
void    HAL_Motor_Disable();
void    HAL_Motor_SetDirection(uint8_t dir);
void    HAL_Motor_StepPulse();       // Fire exactly one step pulse
bool    HAL_Motor_ReadAlarm();       // true = driver fault detected

#endif // HAL_MOTOR_H
