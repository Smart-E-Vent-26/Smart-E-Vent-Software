// ===========================================================
// Kinematics.h — Service Layer: S-Curve Motion Profiles
// Smart E-Ventilator Firmware v2.0
//
// Validated against Motor_Torque_Test.ino:
//   - 30/40/30 spatial profile (accel/cruise/decel)
//   - Frequency-domain linear interpolation (smooth S-Curve)
//   - Separate inhale/exhale cruise intervals
//   - Dynamic kinematics from clinical settings
// ===========================================================
#ifndef KINEMATICS_H
#define KINEMATICS_H

#include <Arduino.h>

// =============================================================
// PROFILE TUNING CONSTANTS (validated on hardware)
// =============================================================
#define KIN_ACCEL_FRACTION          0.30f   // 30% of steps = acceleration
#define KIN_CRUISE_FRACTION         0.40f   // 40% of steps = cruise
#define KIN_DECEL_FRACTION          0.30f   // 30% of steps = deceleration

#define KIN_MIN_STEP_INTERVAL_US    50      // Fastest safe speed (µs)
#define KIN_MAX_STEP_INTERVAL_US    4000    // Slowest safe speed (µs)
#define KIN_CALIBRATE_INTERVAL_US   1200    // Slow homing speed
#define KIN_DEFAULT_CRUISE_US       900     // Default cruise interval

// =============================================================
// MOVE PROFILE STRUCTURE
// =============================================================
typedef struct {
    int32_t  targetSteps;        // Total steps for this move
    int32_t  stepsCompleted;     // Steps fired so far
    int32_t  accelSteps;         // Steps in acceleration phase
    int32_t  decelSteps;         // Steps in deceleration phase
    uint32_t cruiseIntervalUs;   // Step interval during cruise (µs)
    uint32_t lastStepTimeUs;     // micros() timestamp of last step
    bool     active;             // Is a move in progress?
} MoveProfile;

// =============================================================
// PUBLIC API — Move Planning
// =============================================================
void    Kin_Init();

// Plan an S-Curve profiled move with a specific cruise interval.
void    Kin_PlanMove(int32_t totalSteps, uint32_t cruiseUs);

// Plan a constant-speed move (used during calibration/homing).
void    Kin_PlanConstantMove(uint32_t intervalUs);

// Call every fast-loop tick.  Returns true if a step was fired.
bool    Kin_Update();

// Immediately abort the current move.
void    Kin_Stop();

// Query whether the move has finished.
bool    Kin_IsComplete();

// How many steps have been completed in the current / last move.
int32_t Kin_GetStepsCompleted();

// =============================================================
// PUBLIC API — Dynamic Kinematics (links clinical → motor)
// =============================================================
// Recalculate stroke steps and cruise intervals from clinical settings.
// Must be called whenever BPM, I:E, or Tidal Volume change.
void     Kin_UpdateDynamics(uint8_t bpm, float ieRatio, float targetTV_mL);

// Getters for computed values
uint32_t Kin_GetInhaleCruiseUs();
uint32_t Kin_GetExhaleCruiseUs();
int32_t  Kin_GetStrokeSteps();

// Convenience: plan using pre-computed cruise intervals
void     Kin_PlanInhale(int32_t totalSteps);
void     Kin_PlanExhale(int32_t totalSteps);

#endif // KINEMATICS_H
