// ===========================================================
// FSM_App.h — Application Layer: Ventilator State Machine
// Smart E-Ventilator Firmware v2.0
//
// Changes from v1.0:
//   - Added STATE_HOLD (inspiratory plateau) and STATE_PAUSE
//   - Tidal volume in mL instead of raw steps
//   - Step-based volume reporting (not flow-integrated)
//   - Dynamic kinematics from clinical settings
//   - User-triggered homing calibration
// ===========================================================
#ifndef FSM_APP_H
#define FSM_APP_H

#include <Arduino.h>

// =============================================================
// VENTILATOR STATES
// =============================================================
typedef enum {
    STATE_BOOT,         // Power-on, waiting for user to trigger calibration
    STATE_CALIBRATE,    // Homing via Hall sensor (user-triggered)
    STATE_READY,        // Calibrated, waiting to start ventilation
    STATE_INHALE,       // Motor compressing Ambu bag (S-Curve profile)
    STATE_HOLD,         // Inspiratory hold / plateau (motor stationary)
    STATE_EXHALE,       // Motor retracting (S-Curve profile)
    STATE_PAUSE,        // Expiratory pause (motor at home)
    STATE_SOFT_STOP_WAIT, // Pausing 1.5s before retracting
    STATE_RETRACT_HOME, // Slowly retracting to home position
    STATE_FAULT         // Critical alarm — motor disabled
} VentState;

// =============================================================
// VENTILATION MODES
// =============================================================
typedef enum {
    MODE_VCV,   // Volume Control Ventilation
    MODE_PCV    // Pressure Control Ventilation
} VentMode;

// =============================================================
// VENTILATOR SETTINGS
// =============================================================
typedef struct {
    uint8_t  bpm;                   // Breaths per minute (10–30)
    float    ieRatio;               // I:E denominator (e.g. 2.0 → 1:2)
    float    targetTidalVolume_mL;  // VCV target volume in mL (50–600)
    float    targetPIP_kPa;         // PCV target peak pressure (kPa)
    int32_t  maxCompressSteps;      // Physical limit from calibration
} VentSettings;

// =============================================================
// PUBLIC API
// =============================================================
void      FSM_Init();
void      FSM_Update();             // Call every loop() iteration

VentState FSM_GetState();
VentMode  FSM_GetMode();

void      FSM_SetMode(VentMode mode);
void      FSM_SetBPM(uint8_t bpm);
void      FSM_SetIERatio(float ratio);
void      FSM_SetTidalVolumeMl(float ml);
void      FSM_SetTargetPIP(float kpa);

void      FSM_StartVentilation();   // READY -> INHALE
void      FSM_SoftStopVentilation(); // Any -> WAIT -> RETRACT -> READY
void      FSM_EmergencyStop();      // Any -> BOOT (Requires Homing)
void      FSM_StartCalibration();   // BOOT  -> CALIBRATE (user-triggered)

// Getters for telemetry / serial display
const VentSettings* FSM_GetSettings();
float     FSM_GetCurrentPressure();
float     FSM_GetCurrentFlowLPM();
float     FSM_GetDeliveredVolumeMl();   // Steps / STEPS_PER_ML
uint32_t  FSM_GetInhaleTimeMs();
uint32_t  FSM_GetExhaleTimeMs();
void      FSM_SetGraphMode(bool enabled);

#endif // FSM_APP_H
