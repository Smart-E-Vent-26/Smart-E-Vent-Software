// ===========================================================
// Filters.h — Service Layer: Signal Processing
// Smart E-Ventilator Firmware v2.0
// ===========================================================
#ifndef FILTERS_H
#define FILTERS_H

#include <Arduino.h>

// =============================================================
// Exponential Moving Average (EMA) Filter
// =============================================================
typedef struct {
    float alpha;          // Smoothing factor (0.0–1.0). Lower = smoother.
    float value;          // Current filtered output
    bool  initialized;    // First-sample flag
} EMA_Filter;

void  Filter_EMA_Init(EMA_Filter* f, float alpha);
float Filter_EMA_Update(EMA_Filter* f, float rawValue);
float Filter_EMA_GetValue(const EMA_Filter* f);

// =============================================================
// Physical Conversion Utilities
// =============================================================
float Filter_VoltageToKpa(float voltage);    // MPX5010DP voltage -> kPa
float Filter_KpaToFlowLPM(float kpa);       // Venturi Bernoulli -> L/min

// =============================================================
// Direct ADC-based Flow Calculation (validated from test code)
// Uses dead zone + sqrt-based Bernoulli without Venturi tube.
// =============================================================
#define FLOW_DEAD_ZONE_ADC   1.0f     // Suppress noise below this ADC delta
#define FLOW_K_FACTOR        6.09f    // Calibrated: L/min per sqrt(Pa)

float Filter_AdcToFlowLPM(float deltaADC);  // ADC delta -> L/min

// =============================================================
// Venturi Tube Configuration  (D1 = 22 mm, D2 = 12 mm)
// =============================================================
// Pre-computed constant K such that Q(L/min) = K * sqrt(deltaP_kPa)
// Derivation in Filters.cpp
extern const float VENTURI_K;

#endif // FILTERS_H
