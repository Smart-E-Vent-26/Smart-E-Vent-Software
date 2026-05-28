// ===========================================================
// HAL_Sensors.cpp — Hardware Abstraction Layer: Sensor Reads
// Smart E-Ventilator Firmware v2.0
//
// Adds BMP280 dual differential pressure and flow auto-zero,
// matching validated Motor_Torque_Test.ino logic.
// ===========================================================
#include "HAL_Sensors.h"
#include "HAL_Board.h"
#include <Wire.h>
#include <Adafruit_BMP280.h>

// =============================================================
// BMP280 PRESSURE SENSORS (I2C via Level Shifter)
// =============================================================
// Sensor 1 (Ambient): SDO→GND  = address 0x76
// Sensor 2 (Airway):  SDO→3.3V = address 0x77
static Adafruit_BMP280 _bmpAmbient;   // 0x76
static Adafruit_BMP280 _bmpAirway;    // 0x77
static bool _bmpAmbientOk = false;
static bool _bmpAirwayOk  = false;

// =============================================================
// FLOW SENSOR AUTO-ZERO STATE
// =============================================================
static float _flowZeroADC = 56.0f;    // Default baseline (overridden by auto-zero)

// =============================================================
// INIT
// =============================================================
void HAL_Sensors_Init() {
    // Analog pins do not require pinMode on ATmega328P.
    // Perform a few dummy reads to let the ADC multiplexer settle.
    analogRead(PIN_FLOW_SENSOR);
    analogRead(PIN_HALL_SENSOR);
    delay(10);
}

// =============================================================
// BMP280 Pressure — Init
// Wire.begin() must be called before this (done in HAL_Board_Init)
// =============================================================
bool HAL_Sensors_InitPressure() {
    Serial.print(F("[INIT] BMP280 Ambient (0x76): "));
    _bmpAmbientOk = _bmpAmbient.begin(0x76);
    Serial.println(_bmpAmbientOk ? F("OK") : F("NOT FOUND"));

    Serial.print(F("[INIT] BMP280 Airway  (0x77): "));
    _bmpAirwayOk = _bmpAirway.begin(0x77);
    Serial.println(_bmpAirwayOk ? F("OK") : F("NOT FOUND"));

    if (_bmpAmbientOk) {
        _bmpAmbient.setSampling(Adafruit_BMP280::MODE_NORMAL,
                                Adafruit_BMP280::SAMPLING_X2,
                                Adafruit_BMP280::SAMPLING_X16,
                                Adafruit_BMP280::FILTER_X4,
                                Adafruit_BMP280::STANDBY_MS_63);
    }
    if (_bmpAirwayOk) {
        _bmpAirway.setSampling(Adafruit_BMP280::MODE_NORMAL,
                               Adafruit_BMP280::SAMPLING_X2,
                               Adafruit_BMP280::SAMPLING_X16,
                               Adafruit_BMP280::FILTER_X4,
                               Adafruit_BMP280::STANDBY_MS_63);
    }

    return (_bmpAmbientOk && _bmpAirwayOk);
}

// =============================================================
// BMP280 Pressure — Differential Read
// Returns (airway - ambient) in kPa
// =============================================================
float HAL_Sensors_ReadPressureKpa() {
    static float lastGoodKpa = 0.0f;

    if (_bmpAmbientOk && _bmpAirwayOk) {
        float pAirway  = _bmpAirway.readPressure();     // Pa
        float pAmbient = _bmpAmbient.readPressure();    // Pa
        float currentKpa = (pAirway - pAmbient) / 1000.0f; // kPa

        // --- SOFTWARE EMI SHIELD ---
        // 8.0 kPa is ~81 cmH2O. Human lungs pop at 40 cmH2O. 
        // Anything above 8.0 or below -2.0 is a physically impossible ghost spike.
        if (currentKpa > 8.0f || currentKpa < -2.0f) {
            return lastGoodKpa; // Ignore the EMI noise, return the last safe reading!
        }
        
        lastGoodKpa = currentKpa; 
        return currentKpa;
    }
    return 0.0f;
}

bool HAL_Sensors_IsPressureOk() {
    return (_bmpAmbientOk && _bmpAirwayOk);
}

// =============================================================
// Flow Sensor (MPX5010DP on A0)
// =============================================================
uint16_t HAL_Sensors_ReadFlowRaw() {
    return analogRead(PIN_FLOW_SENSOR);
}

float HAL_Sensors_ReadFlowVoltage() {
    return (float)HAL_Sensors_ReadFlowRaw() * ADC_TO_VOLTS;
}

// =============================================================
// Flow Sensor — Auto-Zero (validated from Motor_Torque_Test)
// 3s warm-up → 40ms average to establish resting ADC baseline
// =============================================================
void HAL_Sensors_AutoZeroFlow() {
    Serial.println(F("\n--- FLOW SENSOR WARM-UP (3s) ---"));
    for (int i = 0; i < 3000; i++) {
        analogRead(PIN_FLOW_SENSOR);
        delay(1);
    }

    Serial.println(F("--- AUTO-ZERO ---"));
    unsigned long zStart = micros();
    unsigned long zSum   = 0;
    int zCount = 0;
    while (micros() - zStart < 40000UL) {
        zSum += analogRead(PIN_FLOW_SENSOR);
        zCount++;
    }
    _flowZeroADC = (float)zSum / zCount;
    Serial.print(F("Zero ADC=")); Serial.println(_flowZeroADC);
}

float HAL_Sensors_GetFlowZero() {
    return _flowZeroADC;
}

// =============================================================
// Hall Effect Sensor (AT3503 on A2)
// =============================================================
uint16_t HAL_Sensors_ReadHallRaw() {
    return analogRead(PIN_HALL_SENSOR);
}

bool HAL_Sensors_IsHallTriggered() {
    // A3144 + 10K pull-up: LOW ADC value = magnet detected
    uint16_t val = HAL_Sensors_ReadHallRaw();
    return (val < HALL_TRIGGER_THRESHOLD);
}
