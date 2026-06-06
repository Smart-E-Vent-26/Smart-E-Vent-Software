// ===========================================================
// Kinematics.cpp — Service Layer: S-Curve Motion Profiles
// Smart E-Ventilator Firmware v2.0
//
// S-Curve profile uses frequency-domain linear interpolation:
//   - Converts step intervals to frequencies (steps/sec)
//   - Linearly ramps frequency during accel/decel zones
//   - Converts back to intervals for step timing
//
// This produces smoother mechanical transitions than direct
// interval interpolation (validated on hardware).
// ===========================================================
#include "Kinematics.h"
#include "HAL_Board.h"
#include "HAL_Motor.h"

// =============================================================
// PRIVATE STATE — Move Profile
// =============================================================
static MoveProfile _move;

// =============================================================
// PRIVATE STATE — Dynamic Kinematics
// =============================================================
static uint32_t _inhaleCruiseUs = KIN_DEFAULT_CRUISE_US;
static uint32_t _exhaleCruiseUs = KIN_DEFAULT_CRUISE_US;
static int32_t  _strokeSteps    = 0;

// =============================================================
// INIT
// =============================================================
void Kin_Init() {
    memset(&_move, 0, sizeof(_move));
    _move.active = false;
}

// =============================================================
// _computeCurrentInterval — S-Curve (frequency-domain ramp)
//
// Matches validated _calcStepInterval() from Motor_Torque_Test:
//   30% accel → ramp frequency from minFreq to cruiseFreq
//   40% cruise → constant cruiseFreq
//   30% decel → ramp frequency from cruiseFreq to minFreq
// =============================================================
static uint32_t _computeCurrentInterval() {
    int32_t s = _move.stepsCompleted;
    int32_t total = _move.targetSteps;

    float minFreq    = 1000000.0f / KIN_MAX_STEP_INTERVAL_US;
    float targetFreq = 1000000.0f / _move.cruiseIntervalUs;

    // --- Acceleration phase (first 30%) ---
    if (s < _move.accelSteps && _move.accelSteps > 0) {
        float progress = (float)s / (float)_move.accelSteps;
        float currentFreq = minFreq + (targetFreq - minFreq) * progress;
        return (uint32_t)(1000000.0f / currentFreq);
    }

    // --- Deceleration phase (last 30%) ---
    int32_t decelStart = total - _move.decelSteps;
    if (s >= decelStart && _move.decelSteps > 0) {
        int32_t stepsIntoDecel = s - decelStart;
        float progress = (float)stepsIntoDecel / (float)_move.decelSteps;
        float currentFreq = targetFreq - (targetFreq - minFreq) * progress;
        return (uint32_t)(1000000.0f / currentFreq);
    }

    // --- Cruise phase (middle 40%) ---
    return _move.cruiseIntervalUs;
}

// =============================================================
// Kin_PlanMove — S-Curve profiled move
//
// Plans a move of totalSteps with the given cruise interval.
// The S-Curve automatically handles accel/decel ramps.
// =============================================================
void Kin_PlanMove(int32_t totalSteps, uint32_t cruiseUs) {
    if (totalSteps <= 0) return;

    _move.targetSteps    = totalSteps;
    _move.stepsCompleted = 0;

    // 30/40/30 spatial profile
    _move.accelSteps = (totalSteps * 3) / 10;
    _move.decelSteps = (totalSteps * 3) / 10;
    if (_move.accelSteps < 1) _move.accelSteps = 1;
    if (_move.decelSteps < 1) _move.decelSteps = 1;

    // Cruise interval — clamp to safe bounds
    _move.cruiseIntervalUs = cruiseUs;
    if (_move.cruiseIntervalUs < KIN_MIN_STEP_INTERVAL_US)
        _move.cruiseIntervalUs = KIN_MIN_STEP_INTERVAL_US;
    if (_move.cruiseIntervalUs > KIN_MAX_STEP_INTERVAL_US)
        _move.cruiseIntervalUs = KIN_MAX_STEP_INTERVAL_US;

    _move.lastStepTimeUs = HAL_GetMicros();
    _move.active         = true;
}

// =============================================================
// Kin_PlanConstantMove — Constant speed, no ramp
// Used for calibration/homing. Runs until Kin_Stop().
// =============================================================
void Kin_PlanConstantMove(uint32_t intervalUs) {
    _move.targetSteps    = 999999;   // effectively infinite
    _move.stepsCompleted = 0;
    _move.accelSteps     = 0;
    _move.decelSteps     = 0;
    _move.cruiseIntervalUs = intervalUs;
    _move.lastStepTimeUs   = HAL_GetMicros();
    _move.active           = true;
}

// =============================================================
// Kin_Update — Call from the fast loop.
// Returns true if a step was fired this tick.
// =============================================================
bool Kin_Update() {
    if (!_move.active) return false;

    if (_move.stepsCompleted >= _move.targetSteps) {
        _move.active = false;
        return false;
    }

    uint32_t now      = HAL_GetMicros();
    uint32_t interval = _computeCurrentInterval();

    if ((now - _move.lastStepTimeUs) >= interval) {
        HAL_Motor_StepPulse();
        _move.lastStepTimeUs = now;
        _move.stepsCompleted++;
        return true;
    }
    return false;
}

// =============================================================
// Utility
// =============================================================
void    Kin_Stop()              { _move.active = false; }
bool    Kin_IsComplete()        { return !_move.active; }
int32_t Kin_GetStepsCompleted() { return _move.stepsCompleted; }

uint32_t Kin_GetCurrentIntervalUs() {
    return _move.active ? _computeCurrentInterval() : 0;
}

float Kin_GetInstantaneousFlowLPM(bool isExhale) {
    uint32_t interval = Kin_GetCurrentIntervalUs();
    if (interval == 0 || !_move.active) {
        return 0.0f;
    }
    // Flow (L/min) = (Volume_per_Step_mL / intervalUs) * 60,000
    // Volume_per_Step_mL = 1.0f / MECH_STEPS_PER_ML
    float volPerStep = 1.0f / MECH_STEPS_PER_ML;
    float flowLPM = (volPerStep / (float)interval) * 60000.0f;
    
    return isExhale ? -flowLPM : flowLPM;
}

// =============================================================
// DYNAMIC KINEMATICS — Links clinical settings to motor params
//
// Direct port of updateMotorKinematics() from Motor_Torque_Test
// =============================================================
void Kin_UpdateDynamics(uint8_t bpm, float ieRatio, float targetTV_mL) {
    // 1. Calculate stroke steps from target volume
    _strokeSteps = (int32_t)(targetTV_mL * MECH_STEPS_PER_ML);

    // Safety clamp
    if (_strokeSteps > MECH_FULL_COMPRESS_STEPS)
        _strokeSteps = MECH_FULL_COMPRESS_STEPS;
    if (_strokeSteps < 50)
        _strokeSteps = 50;

    // 2. Calculate timing
    float breathPeriodSec = 60.0f / bpm;
    float inhaleSec = breathPeriodSec / (1.0f + ieRatio);
    float exhaleSec = breathPeriodSec - inhaleSec;

    float targetCompressionSec = inhaleSec * MECH_INHALE_MOTION_FRACTION;
    float targetRetractionSec  = exhaleSec * MECH_INHALE_MOTION_FRACTION;

    // 3. Calculate required speeds
    // Based on 30/40/30 spatial profile, effective distance = 1.6 × total steps
    float reqInhaleStepsPerSec = (1.6f * _strokeSteps) / targetCompressionSec;
    float reqExhaleStepsPerSec = (1.6f * _strokeSteps) / targetRetractionSec;

    // 4. Convert to microsecond intervals
    if (reqInhaleStepsPerSec > 0)
        _inhaleCruiseUs = (uint32_t)(1000000.0f / reqInhaleStepsPerSec);
    else
        _inhaleCruiseUs = KIN_MAX_STEP_INTERVAL_US;

    if (reqExhaleStepsPerSec > 0)
        _exhaleCruiseUs = (uint32_t)(1000000.0f / reqExhaleStepsPerSec);
    else
        _exhaleCruiseUs = KIN_MAX_STEP_INTERVAL_US;

    // Clamp to safe bounds
    if (_inhaleCruiseUs > KIN_MAX_STEP_INTERVAL_US) _inhaleCruiseUs = KIN_MAX_STEP_INTERVAL_US;
    if (_inhaleCruiseUs < KIN_MIN_STEP_INTERVAL_US) _inhaleCruiseUs = KIN_MIN_STEP_INTERVAL_US;
    if (_exhaleCruiseUs > KIN_MAX_STEP_INTERVAL_US) _exhaleCruiseUs = KIN_MAX_STEP_INTERVAL_US;
    if (_exhaleCruiseUs < KIN_MIN_STEP_INTERVAL_US) _exhaleCruiseUs = KIN_MIN_STEP_INTERVAL_US;
}

uint32_t Kin_GetInhaleCruiseUs() { return _inhaleCruiseUs; }
uint32_t Kin_GetExhaleCruiseUs() { return _exhaleCruiseUs; }
int32_t  Kin_GetStrokeSteps()    { return _strokeSteps; }

// =============================================================
// Convenience: plan with pre-computed cruise intervals
// =============================================================
void Kin_PlanInhale(int32_t totalSteps) {
    Kin_PlanMove(totalSteps, _inhaleCruiseUs);
}

void Kin_PlanExhale(int32_t totalSteps) {
    Kin_PlanMove(totalSteps, _exhaleCruiseUs);
}

void Kin_SetCruiseInterval(uint32_t intervalUs) {
    if (intervalUs < KIN_MIN_STEP_INTERVAL_US) intervalUs = KIN_MIN_STEP_INTERVAL_US;
    if (intervalUs > KIN_MAX_STEP_INTERVAL_US) intervalUs = KIN_MAX_STEP_INTERVAL_US;
    _move.cruiseIntervalUs = intervalUs;
}
