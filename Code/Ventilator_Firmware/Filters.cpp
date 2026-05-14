// ===========================================================
// Filters.cpp — Service Layer: Signal Processing
// Smart E-Ventilator Firmware v2.0
// ===========================================================
#include "Filters.h"
#include "HAL_Sensors.h"
#include <math.h>

// =============================================================
// EMA Filter
// =============================================================
void Filter_EMA_Init(EMA_Filter* f, float alpha) {
    f->alpha       = alpha;
    f->value       = 0.0f;
    f->initialized = false;
}

float Filter_EMA_Update(EMA_Filter* f, float rawValue) {
    if (!f->initialized) {
        f->value       = rawValue;
        f->initialized = true;
    } else {
        f->value = f->alpha * rawValue + (1.0f - f->alpha) * f->value;
    }
    return f->value;
}

float Filter_EMA_GetValue(const EMA_Filter* f) {
    return f->value;
}

// =============================================================
// MPX5010DP Transfer Function
// Datasheet:  Vout = Vs * (0.09 * P + 0.04)
// Solved:     P(kPa) = (Vout - offset) / scale
// =============================================================
float Filter_VoltageToKpa(float voltage) {
    float kpa = (voltage - FLOW_SENSOR_OFFSET_V) / FLOW_SENSOR_SCALE;
    return (kpa < 0.0f) ? 0.0f : kpa;
}

// =============================================================
// Direct ADC-based Flow Calculation (validated from test code)
//
// Uses a dead zone to suppress sensor noise, then converts
// ADC delta to differential pressure and applies sqrt-based
// Bernoulli equation with calibrated K_FLOW constant.
// =============================================================
float Filter_AdcToFlowLPM(float deltaADC) {
    if (fabsf(deltaADC) < FLOW_DEAD_ZONE_ADC) {
        return 0.0f;
    }

    // Subtract dead zone from effective delta
    float effectiveADC = (deltaADC > 0)
        ? (deltaADC - FLOW_DEAD_ZONE_ADC)
        : (deltaADC + FLOW_DEAD_ZONE_ADC);

    float dV     = effectiveADC * ADC_TO_VOLTS;
    float dP_kPa = dV / FLOW_SENSOR_SCALE;
    float sign   = (dP_kPa > 0) ? 1.0f : -1.0f;

    // K_FLOW calibrated for Pascals, so multiply kPa by 1000 before sqrt
    return sign * FLOW_K_FACTOR * sqrtf(fabsf(dP_kPa) * 1000.0f);  // L/min
}

// =============================================================
// Venturi Tube Flow Calculation (Bernoulli's Principle)
//
// Given:
//   D1 = 22 mm  (wide section)    -> A1 = pi * (0.011)^2 = 3.8013e-4 m^2
//   D2 = 12 mm  (narrow section)  -> A2 = pi * (0.006)^2 = 1.1310e-4 m^2
//   rho = 1.225 kg/m^3 (air at sea level, 15 C)
//
// Pre-computed constant (see v1.0 derivation):
//   K_final = 188.8 L/min per sqrt(kPa) @ 22mm/10mm, Cd = 0.97
//
// NOTE: This is for use when the Venturi tube is installed.
//       Currently using Filter_AdcToFlowLPM() instead.
// =============================================================
const float VENTURI_K = 188.8f;

float Filter_KpaToFlowLPM(float kpa) {
    if (kpa <= 0.0f) return 0.0f;
    return VENTURI_K * sqrtf(kpa);
}
