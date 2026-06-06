// ===========================================================
// HAL_Board.cpp — Hardware Abstraction Layer: Board & Pin Defs
// Smart E-Ventilator Firmware v2.0
// ===========================================================
#include "HAL_Board.h"
#include <Wire.h>
#include <avr/wdt.h>

// =============================================================
// HAL_Board_Init — Set pin modes and default states
// =============================================================
void HAL_Board_Init() {
    // Motor driver pins
    pinMode(PIN_MOTOR_PUL, OUTPUT);
    pinMode(PIN_MOTOR_DIR, OUTPUT);
    pinMode(PIN_MOTOR_ENA, OUTPUT);
    pinMode(PIN_MOTOR_ALM, INPUT_PULLUP);

    // User interface pins
    pinMode(PIN_BUZZER,     OUTPUT);
    pinMode(PIN_LED_GREEN,  OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_RED,    OUTPUT);
    pinMode(PIN_START_STOP, INPUT_PULLUP);
    pinMode(PIN_EMERGENCY_STOP, INPUT_PULLUP);

    // Analog sensor pins (A0, A2) do not require pinMode on ATmega328P

    // Set all outputs to safe defaults
    digitalWrite(PIN_MOTOR_PUL, LOW);
    digitalWrite(PIN_MOTOR_DIR, LOW);
    digitalWrite(PIN_MOTOR_ENA, HIGH);  // CS-D508: HIGH = disabled (safe boot)
    digitalWrite(PIN_BUZZER,    LOW);
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_YELLOW,LOW);
    digitalWrite(PIN_LED_RED,   LOW);

    // Initialize I2C bus for BMP280 pressure sensors
    Wire.begin();
    Wire.setWireTimeout(25000, true);   // 25ms timeout, auto-reset on hang
}

// =============================================================
// Time Wrappers
// =============================================================
uint32_t HAL_GetMillis() { return millis(); }
uint32_t HAL_GetMicros() { return micros(); }

// =============================================================
// Watchdog Timer (ATmega328P avr/wdt.h)
// Increased to 500ms to accommodate BMP280 I2C timeouts.
// =============================================================
void HAL_WDT_Enable()  { wdt_enable(WDTO_500MS); }
void HAL_WDT_Reset()   { wdt_reset(); }
void HAL_WDT_Disable() { wdt_disable(); }

void HAL_WDT_ForceReboot() {
    wdt_enable(WDTO_15MS);
    while (1) {}
}
