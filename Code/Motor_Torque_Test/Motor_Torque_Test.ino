// ===========================================================
// Motor_Torque_Test.ino — Breath Sequence & Torque Test
// Smart E-Ventilator — Pre-Integration Testing
//
// Pressure monitoring via dual BMP280 (differential: airway - ambient)
//
// This sketch delivers single, manually-triggered breaths
// that match real ventilator waveforms (VCV and PCV).
//
// Motor: CS-M22331 NEMA23 (closed-loop, 1000-line encoder)
// Driver: CS-D508  (closed-loop, ALM+ = position error)
//
// SERIAL COMMANDS (115200 baud):
//   C  — Deliver one breath (VCV or PCV based on mode)
//   V  — Switch to VCV mode
//   P  — Switch to PCV mode
//   F  — Manual compress (strokeSteps forward)
//   B  — Manual retract to home
//   H  — Home sequence (use Hall sensor magnet)
//   +/-  — Stroke ±50 steps
//   >/<  — Speed cruise interval ±50µs
//   1-5  — Quick stroke: 10/25/50/75/100%
//   A  — Toggle ALM+ monitoring
//   D  — Disable motor (release holding torque)
//   X  — EMERGENCY STOP
//   ?  — Print status
//   Parameterised:
//     B<nn>  — Set BPM (e.g. B15)
//     R<nn>  — Set I:E ratio (e.g. R20 → 1:2.0)
//     T<nnn> — Set tidal volume steps (e.g. T400)
//     I<nn>  — Set PIP target x0.1 kPa (e.g. I25 → 2.5 kPa)
// ===========================================================

// =============================================================
// LIBRARIES
// =============================================================
#include <Wire.h>
#include <Adafruit_BMP280.h>

// =============================================================
// PIN DEFINITIONS
// =============================================================
#define PIN_MOTOR_PUL       2
#define PIN_MOTOR_DIR       3
#define PIN_MOTOR_ENA       4
#define PIN_MOTOR_ALM       9

#define PIN_BUZZER          5
#define PIN_LED_GREEN       6
#define PIN_LED_YELLOW      7
#define PIN_LED_RED         8

#define PIN_FLOW_SENSOR     A0    // MPX5010DP
#define PIN_HALL_SENSOR     A2    // A3144

// =============================================================
// MOTOR & MECHANICAL CONSTANTS
// =============================================================
#define MOTOR_DIR_COMPRESS  LOW
#define MOTOR_DIR_RETRACT   HIGH
#define PULSES_PER_REV      800      // 800 microstepping (DIP: SW1=OFF SW2=ON SW3=ON SW4=ON)
#define FULL_COMPRESS_STEPS 1300     // Calibrated: change ONLY this value

#define MIN_INTERVAL_US     50      // Fastest safe speed (was 100)
#define MAX_INTERVAL_US     4000    // Slowest safe speed (very soft start/stop)
#define CRUISE_INTERVAL_US  900     // Default cruise speed for breaths

// Calibration & Kinematics Constants
const float STEPS_PER_ML = 2.166667f;      // 1300 steps / 600 mL
const float INHALE_MOTION_FRACTION = 0.8f; // 80% compression, 20% hold

// Hall sensor
#define HALL_TRIGGER_THRESHOLD  512

// =============================================================
// BREATH PROFILE CONSTANTS
// =============================================================
#define ACCEL_FRACTION      0.30f   // 30% of steps = acceleration (longer S-Curve ease in)
#define CRUISE_FRACTION     0.40f   // 40% of steps = cruise
#define DECEL_FRACTION      0.30f   // 30% of steps = deceleration (longer S-Curve ease out)
#define TELEMETRY_EVERY_N   40      // Print status every N steps
#define SETTLE_STEPS        150     // Steps crossing the mechanical gap before bag contact

// Default tidal volume = 80% of full compression
#define DEFAULT_TIDAL_STEPS ((int32_t)(FULL_COMPRESS_STEPS * 0.80f))

// Stall detection: if we fire this many consecutive steps
// without pressure changing by STALL_PRESSURE_DELTA_KPA,
// declare a stall.
#define STALL_CHECK_STEPS       80
#define STALL_PRESSURE_DELTA_KPA 0.05f

// =============================================================
// STATE
// =============================================================
// Ventilation settings
static uint8_t  bpm             = 15;
static float    ieRatio         = 2.0f;     // 1:2
static float    targetTidalVolume_mL = 400.0f;
static int32_t  tidalSteps      = (int32_t)(400.0f * 1.7333f);
static float    targetPIP_kPa   = 2.5f;     // ~25 cmH2O
static bool     vcvMode         = true;     // true=VCV, false=PCV

// Motor state
static int32_t  currentPosition = 0;
static int32_t  strokeSteps     = DEFAULT_TIDAL_STEPS;
static uint32_t inhaleCruiseUs  = CRUISE_INTERVAL_US;
static uint32_t exhaleCruiseUs  = CRUISE_INTERVAL_US;
static bool     motorEnabled    = false;
static bool     isHomed         = false;
static bool     almEnabled      = false;    // ALM+ off by default
static bool     faultDetected   = false;
static bool     breathActive    = false;    // Prevents commands during breath

// For manual moves
static bool     moveActive      = false;
static int32_t  moveTarget      = 0;
static int32_t  moveCompleted   = 0;
static uint8_t  moveDirection   = MOTOR_DIR_COMPRESS;
static uint32_t lastStepUs      = 0;

// =============================================================
// BMP280 PRESSURE SENSORS (I2C via Level Shifter)
// =============================================================
// Sensor 1 (Ambient): SDO→GND  = address 0x76
// Sensor 2 (Airway):  SDO→3.3V = address 0x77
Adafruit_BMP280 bmpAmbient;   // 0x76
Adafruit_BMP280 bmpAirway;    // 0x77
static bool bmpAmbientOk = false;
static bool bmpAirwayOk  = false;
static float lastPressure_kPa = 0.0f;  // Cached differential pressure

// =============================================================
// FLOW SENSOR STATE
// =============================================================
static unsigned long flowStartUs = 0;
static unsigned long flowSum = 0;
static unsigned int flowCount = 0;
static float zeroF = 56.0f;
static float lastRaw = 56.0f;
static float lastRawADC = 0.0f;     // For telemetry CSV output
static float lastVoltage = 0.0f;     // For telemetry CSV output
static float emaDerivative = 0.0f;
static int quietSamples = 0;
static float emaFlow = 0.0f;
static float tidalVolume_mL = 0.0f;
static float breathZeroF = 56.0f;    // Dynamic per-breath baseline (captured during settle)
static bool  settled = false;         // Whether the settle phase is complete
bool isMotorStepping = false;

// =============================================================
// FORWARD DECLARATIONS
// =============================================================
static void _handleCommand(char cmd);
static int32_t _readSerialInt();
static void _enableMotor();
static void _disableMotor();
static void _emergencyStop();
static void _homeSequence();
static void _printStatus();
static void _printStrokeSet();
static void _buzzWarning();
static void _buzzError();

static float _readPressureKpa();
static uint32_t _calcStepInterval(int32_t step, int32_t totalSteps, uint32_t targetCruiseUs);
static bool _checkAbort();
static void _fireStep();

static void _executeVCVBreath();
static void _executePCVBreath();

static void updateMotorKinematics();

// =============================================================
// KINEMATICS UPDATE
// =============================================================
void updateMotorKinematics() {
    // 1. Calculate Stroke Steps
    strokeSteps = (int32_t)(targetTidalVolume_mL * STEPS_PER_ML);
    
    // Safety limit clamp
    if (strokeSteps > FULL_COMPRESS_STEPS) strokeSteps = FULL_COMPRESS_STEPS;
    if (strokeSteps < 50) strokeSteps = 50; // minimum movement
    tidalSteps = strokeSteps;

    // 2. Calculate Timings
    float breathPeriodSec = 60.0f / bpm;
    float inhaleSec = breathPeriodSec / (1.0f + ieRatio);
    float exhaleSec = breathPeriodSec - inhaleSec;
    
    float targetCompressionSec = inhaleSec * INHALE_MOTION_FRACTION;
    float targetRetractionSec = exhaleSec * INHALE_MOTION_FRACTION; // Use same motion fraction for exhale to reflect I:E perfectly
    
    // 3. Calculate Required Speeds
    // Based on 30/40/30 spatial profile, effective distance = 1.6 * total steps
    float reqInhaleStepsPerSec = (1.6f * strokeSteps) / targetCompressionSec;
    float reqExhaleStepsPerSec = (1.6f * strokeSteps) / targetRetractionSec;
    
    // 4. Convert to Microsecond Interval
    if (reqInhaleStepsPerSec > 0) {
        inhaleCruiseUs = (uint32_t)(1000000.0f / reqInhaleStepsPerSec);
    } else {
        inhaleCruiseUs = MAX_INTERVAL_US;
    }

    if (reqExhaleStepsPerSec > 0) {
        exhaleCruiseUs = (uint32_t)(1000000.0f / reqExhaleStepsPerSec);
    } else {
        exhaleCruiseUs = MAX_INTERVAL_US;
    }
    
    // Clamp to min/max
    if (inhaleCruiseUs > MAX_INTERVAL_US) inhaleCruiseUs = MAX_INTERVAL_US;
    if (inhaleCruiseUs < MIN_INTERVAL_US) inhaleCruiseUs = MIN_INTERVAL_US;
    if (exhaleCruiseUs > MAX_INTERVAL_US) exhaleCruiseUs = MAX_INTERVAL_US;
    if (exhaleCruiseUs < MIN_INTERVAL_US) exhaleCruiseUs = MIN_INTERVAL_US;
}

// =============================================================
// SETUP
// =============================================================
void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    pinMode(PIN_MOTOR_PUL, OUTPUT);
    pinMode(PIN_MOTOR_DIR, OUTPUT);
    pinMode(PIN_MOTOR_ENA, OUTPUT);
    pinMode(PIN_MOTOR_ALM, INPUT_PULLUP);

    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);

    pinMode(PIN_FLOW_SENSOR, INPUT);
    pinMode(PIN_HALL_SENSOR, INPUT);

    digitalWrite(PIN_MOTOR_ENA, HIGH);  // Disabled
    digitalWrite(PIN_MOTOR_PUL, LOW);
    digitalWrite(PIN_MOTOR_DIR, LOW);

    digitalWrite(PIN_LED_GREEN, HIGH);
    digitalWrite(PIN_LED_YELLOW, LOW);
    digitalWrite(PIN_LED_RED, LOW);

    Serial.println(F(""));
    Serial.println(F("============================================"));
    Serial.println(F("   BREATH SEQUENCE & TORQUE TEST"));
    Serial.println(F("   Smart E-Ventilator"));
    Serial.println(F("============================================"));
    Serial.println(F(""));
    Serial.println(F("Motor: CS-M22331 (closed-loop, encoder)"));
    Serial.println(F("Driver: CS-D508  (closed-loop)"));
    Serial.println(F(""));
    Serial.println(F("COMMANDS:"));
    Serial.println(F("  C=Breath  V=VCV  P=PCV  ?=Status  X=Stop"));
    Serial.println(F("  H=Home  F=Fwd  B=Back  D=Disable motor"));
    Serial.println(F("  +/-=Stroke  >/<  =Speed  1-5=Quick%"));
    Serial.println(F("  B<n>=BPM  R<n>=I:E  T<n>=TV  I<n>=PIP"));
    Serial.println(F(""));
    _printStatus();

    // --- Init I2C and BMP280 pressure sensors ---
    Wire.begin();
    Wire.setWireTimeout(25000, true); // 25ms timeout, reset bus on timeout to prevent freezing
    Serial.print(F("[INIT] BMP280 Ambient (0x76): "));
    bmpAmbientOk = bmpAmbient.begin(0x76);
    Serial.println(bmpAmbientOk ? F("OK") : F("NOT FOUND"));

    Serial.print(F("[INIT] BMP280 Airway  (0x77): "));
    bmpAirwayOk = bmpAirway.begin(0x77);
    Serial.println(bmpAirwayOk ? F("OK") : F("NOT FOUND"));

    if (bmpAmbientOk) {
        bmpAmbient.setSampling(Adafruit_BMP280::MODE_NORMAL,
                               Adafruit_BMP280::SAMPLING_X2,
                               Adafruit_BMP280::SAMPLING_X16,
                               Adafruit_BMP280::FILTER_X4,
                               Adafruit_BMP280::STANDBY_MS_63);
    }
    if (bmpAirwayOk) {
        bmpAirway.setSampling(Adafruit_BMP280::MODE_NORMAL,
                              Adafruit_BMP280::SAMPLING_X2,
                              Adafruit_BMP280::SAMPLING_X16,
                              Adafruit_BMP280::FILTER_X4,
                              Adafruit_BMP280::STANDBY_MS_63);
    }

    // Auto-Zero Flow Sensor
    Serial.println(F("\n--- FLOW SENSOR WARM-UP (3s) ---"));
    for (int i = 0; i < 3000; i++) {
        analogRead(PIN_FLOW_SENSOR);
        delay(1);
    }
    Serial.println(F("--- AUTO-ZERO ---"));
    unsigned long zStart = micros();
    unsigned long zSum = 0;
    int zCount = 0;
    while (micros() - zStart < 40000UL) {
        zSum += analogRead(PIN_FLOW_SENSOR);
        zCount++;
    }
    zeroF = (float)zSum / zCount;
    lastRaw = zeroF;
    flowStartUs = micros();
    Serial.print(F("Zero ADC=")); Serial.println(zeroF);

    // Initialize kinematics
    updateMotorKinematics();
}

// =============================================================
// MAIN LOOP
// =============================================================
void loop() {
    // Continuously poll flow sensor in the background
    _accumulateFlowNonBlocking();

    // ALM+ check
    if (almEnabled && !faultDetected && digitalRead(PIN_MOTOR_ALM) == LOW) {
        _emergencyStop();
        faultDetected = true;
        Serial.println(F("\n!!! ALM+ FAULT — position error detected !!!"));
        Serial.println(F("!!! Motor disabled. Check load/mechanism. !!!"));
        _buzzError();
    }

    // Manual move execution (non-blocking)
    if (moveActive) {
        uint32_t now = micros();
        if ((now - lastStepUs) >= inhaleCruiseUs) {
            lastStepUs = now;

            // Enforce compression limit
            if (moveDirection == MOTOR_DIR_COMPRESS && currentPosition >= FULL_COMPRESS_STEPS) {
                moveActive = false;
                Serial.println(F("[SAFETY] Max compression!"));
                _buzzWarning();
                return;
            }

            _fireStep();
            moveCompleted++;
            currentPosition += (moveDirection == MOTOR_DIR_COMPRESS) ? 1 : -1;
            if (currentPosition < 0) currentPosition = 0;

            if (moveCompleted >= moveTarget) {
                moveActive = false;
                Serial.print(F("[DONE] ")); Serial.print(moveCompleted);
                Serial.print(F(" steps | Pos=")); Serial.println(currentPosition);
            }
        }
        digitalWrite(PIN_LED_YELLOW, (millis() / 200) % 2);
    }

    // Serial commands
    if (Serial.available() && !breathActive) {
        char cmd = Serial.read();
        _handleCommand(cmd);
    }
}

// =============================================================
// NON-BLOCKING FLOW ACCUMULATION & DELAY
// =============================================================
static uint32_t currentStepIntervalUs = 0; // Tracks motor speed for EMI cancellation

static void _accumulateFlowNonBlocking() {
    unsigned long now = micros();
    flowSum += analogRead(PIN_FLOW_SENSOR);
    flowCount++;

    if (now - flowStartUs >= 40000UL) {
        float raw = (float)flowSum / flowCount;
        lastRawADC = raw;                          // Store for telemetry
        lastVoltage = raw * (5.0f / 1023.0f);      // Store for telemetry
        
        float dRaw = fabs(raw - lastRaw);
        lastRaw = raw;
        emaDerivative = 0.2f * dRaw + 0.8f * emaDerivative;
        
        // --- ADAPTIVE ZEROING ---
        // Only track baseline drift when the machine is NOT actively breathing.
        // If we track during a breath, it will slowly erase gentle mechanical squeezes!
        if (!breathActive) {
            if (emaDerivative < 2.0f && fabs(raw - zeroF) < 15.0f) {
                quietSamples++;
                if (quietSamples > 12) {
                    zeroF = 0.05f * raw + 0.95f * zeroF;
                }
            } else {
                quietSamples = 0;
            }
        } else {
            quietSamples = 0;
        }

        // --- REMOVED EMI CORRECTION ---
        // Since you installed the 47uF and 0.1uF capacitors, the hardware noise is gone!
        // We no longer need to mathematically add back phantom voltage.
        
        // Use per-breath dynamic baseline during active breathing,
        // or the static zero when idle.
        float activeZero = (breathActive && settled) ? breathZeroF : zeroF;
        float deltaADC = raw - activeZero;
        
        // No negative clamp needed — the dynamic per-breath baseline
        // absorbs EMI/vibration offset, and the dead zone handles residual noise.

        float flowRaw;
        // Reduced DEAD_ZONE to 1.0 to increase sensitivity to the gentle mechanical mechanism
        const float DEAD_ZONE_ADC = 1.0f;
        const float V_PER_ADC = 5.0f / 1023.0f;
        const float V_TO_KPA  = 1.0f / 0.45f;
        const float K_FLOW    = 6.09f;

        if (fabs(deltaADC) < DEAD_ZONE_ADC) {
            flowRaw = 0.0f;
        } else {
            float effectiveADC = (deltaADC > 0) ? (deltaADC - DEAD_ZONE_ADC) : (deltaADC + DEAD_ZONE_ADC);
            float dV = effectiveADC * V_PER_ADC;
            float dP_kPa = dV * V_TO_KPA;
            float sign = (dP_kPa > 0) ? 1.0f : -1.0f;
            
            // K_FLOW was calibrated for Pascals, so we must multiply kPa by 1000 BEFORE sqrt!
            flowRaw = sign * K_FLOW * sqrt(fabs(dP_kPa) * 1000.0f); // L/min
        }

        emaFlow = 0.2f * flowRaw + 0.8f * emaFlow;
        
        // Integrate volume (legacy flow calculation disabled for accurate step-based volume)
        // if (emaFlow > 0.0f) {
        //     tidalVolume_mL += emaFlow * 0.666667f;
        // }

        flowSum = 0;
        flowCount = 0;
        flowStartUs = now;

        // --- Read BMP280 pressure cooperatively (every 40ms, non-blocking) ---
        // This keeps the I2C reads OUT of the tight motor step loop.
        _readPressureKpa();
    }
}

static void _delayWithFlow(uint32_t waitUs) {
    uint32_t start = micros();
    while (micros() - start < waitUs) {
        _accumulateFlowNonBlocking();
    }
}

// Read differential airway pressure from dual BMP280 sensors
static float _readPressureKpa() {
    if (bmpAmbientOk && bmpAirwayOk) {
        float pAirway  = bmpAirway.readPressure();   // Pa
        float pAmbient = bmpAmbient.readPressure();   // Pa
        lastPressure_kPa = (pAirway - pAmbient) / 1000.0f;  // Convert Pa to kPa
        return lastPressure_kPa;
    }
    return 0.0f;
}

// =============================================================
// TRAPEZOIDAL STEP INTERVAL CALCULATOR
//
// Given the current step number and total steps, returns the
// step interval in µs for a trapezoidal speed profile:
//   [0, accelEnd)         → ramp from MAX_INTERVAL to cruise
//   [accelEnd, decelStart)→ cruise at cruiseIntervalUs
//   [decelStart, total)   → ramp from cruise to MAX_INTERVAL
// =============================================================
static uint32_t _calcStepInterval(int32_t step, int32_t totalSteps, uint32_t targetCruiseUs) {
    // 30% Accel, 40% Cruise, 30% Decel
    int32_t accelSteps = (totalSteps * 3) / 10;
    int32_t decelSteps = accelSteps;
    
    float minFreq = 1000000.0f / MAX_INTERVAL_US;
    float targetFreq = 1000000.0f / targetCruiseUs;
    
    if (step < accelSteps) {
        float progress = (float)step / accelSteps;
        float currentFreq = minFreq + (targetFreq - minFreq) * progress;
        return (uint32_t)(1000000.0f / currentFreq);
    } 
    else if (step >= totalSteps - decelSteps) {
        int32_t stepsIntoDecel = step - (totalSteps - decelSteps);
        float progress = (float)stepsIntoDecel / decelSteps;
        float currentFreq = targetFreq - (targetFreq - minFreq) * progress;
        return (uint32_t)(1000000.0f / currentFreq);
    } 
    else {
        return targetCruiseUs;
    }
}

// =============================================================
// ABORT CHECK — returns true if user sent X
// =============================================================
static bool _checkAbort() {
    if (Serial.available()) {
        char c = Serial.peek();
        if (c == 'X' || c == 'x') {
            Serial.read();
            _emergencyStop();
            Serial.println(F("\n[ABORT] Breath aborted!"));
            return true;
        }
    }
    // ALM+ check during breath
    if (almEnabled && digitalRead(PIN_MOTOR_ALM) == LOW) {
        _emergencyStop();
        faultDetected = true;
        Serial.println(F("\n!!! ALM+ FAULT during breath !!!"));
        _buzzError();
        return true;
    }
    return false;
}

// =============================================================
// FIRE ONE STEP PULSE
// =============================================================
static void _fireStep() {
    digitalWrite(PIN_MOTOR_PUL, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_MOTOR_PUL, LOW);
}

// =============================================================
// VCV BREATH — Volume Control Ventilation
//
// Breath Waveform:
//   INHALE: Motor delivers a fixed number of steps (tidalSteps)
//           using a trapezoidal speed profile. After the motor
//           reaches peak compression, it HOLDS position for the
//           remaining inhale time (inspiratory hold/plateau).
//
//   EXHALE: Motor retracts the same number of steps back to home.
//           After retraction, it holds at home for the remaining
//           exhale time (expiratory pause).
//
//   Flow waveform shape: trapezoidal (ramp up, steady, ramp down)
//   Pressure waveform:   rises during inhale, plateaus during
//                         hold, drops to zero during exhale.
// =============================================================
static void _executeVCVBreath() {
    breathActive = true;

    // --- Calculate breath timing ---
    uint32_t breathPeriodMs = 60000UL / bpm;
    uint32_t inhaleMs = breathPeriodMs / (1.0f + ieRatio);
    uint32_t exhaleMs = breathPeriodMs - inhaleMs;
    int32_t steps = tidalSteps;

    // Safety clamp
    if (currentPosition + steps > FULL_COMPRESS_STEPS) {
        steps = FULL_COMPRESS_STEPS - currentPosition;
    }
    if (steps <= 0) {
        Serial.println(F("[ERR] No room to compress!"));
        breathActive = false;
        settled = false;
        return;
    }

    Serial.println(F("\n========== VCV BREATH =========="));
    Serial.print(F("BPM=")); Serial.print(bpm);
    Serial.print(F("  I:E=1:")); Serial.print(ieRatio, 1);
    Serial.print(F("  TV=")); Serial.print(targetTidalVolume_mL, 1);
    Serial.print(F(" mL (")); Serial.print(steps); Serial.println(F(" steps)"));
    Serial.print(F("Inhale=")); Serial.print(inhaleMs);
    Serial.print(F("ms  Exhale=")); Serial.print(exhaleMs);
    Serial.println(F("ms"));
    Serial.println(F("================================"));

    _enableMotor();
    tidalVolume_mL = 0.0f; // Reset volume accumulator
    emaFlow = 0.0f;        // Reset EMA
    breathZeroF = zeroF;   // Start with static zero, will be overridden after settling
    settled = false;       // Mark settling phase as active

    // ===== PHASE 1: INHALE (compress with trapezoidal profile) =====
    Serial.println(F("[INH] Compressing..."));
    digitalWrite(PIN_MOTOR_DIR, MOTOR_DIR_COMPRESS);
    delay(5);

    uint32_t inhaleStartMs = millis();
    isMotorStepping = true;

    for (int32_t i = 0; i < steps; i++) {
        if (_checkAbort()) { breathActive = false; isMotorStepping = false; settled = false; return; }

        // --- Dynamic Per-Breath Baseline ---
        // At the end of the settle zone (mechanical gap before bag contact),
        // capture the current ADC average as the operational baseline.
        if (i == SETTLE_STEPS && !settled) {
            breathZeroF = lastRaw;      // Operational zero under motor conditions
            emaFlow = 0.0f;             // Reset EMA to prevent accumulated drift
            tidalVolume_mL = 0.0f;      // Reset volume (nothing real was measured yet)
            settled = true;
            Serial.print(F("  [SETTLE] breathZero=")); Serial.println(breathZeroF, 1);
        }

        uint32_t interval = _calcStepInterval(i, steps, inhaleCruiseUs);
        currentStepIntervalUs = interval;
        _fireStep();
        currentPosition++;

        // Telemetry every N steps (suppress during settle to avoid transient spikes in CSV)
        if ((i % TELEMETRY_EVERY_N == 0 || i == steps - 1) && settled) {
            const char* phase;
            // 30/40/30 split
            int32_t accelEnd = (steps * 3) / 10;
            int32_t decelStart = steps - (steps * 3) / 10;
            if (i < accelEnd) phase = "accel";
            else if (i >= decelStart) phase = "decel";
            else phase = "cruise";

            Serial.print(F("  INH ")); Serial.print(phase);
            Serial.print(F(" Stp=")); Serial.print(i + 1);
            Serial.print(F("/")); Serial.print(steps);
            Serial.print(F("  Flow=")); Serial.print(emaFlow, 1);
            Serial.print(F(" L/min  Vol=")); Serial.print((i + 1) / STEPS_PER_ML, 1);
            Serial.print(F(" mL"));
            // Use cached pressure (updated every 40ms in flow accumulator)
            Serial.print(F("  Pressure=")); Serial.print(lastPressure_kPa, 3);
            Serial.print(F(" kPa"));
            // Raw ADC and voltage for CSV logging
            Serial.print(F("  FlowADC=")); Serial.print((int)lastRawADC);
            Serial.print(F("  FlowVolt=")); Serial.print(lastVoltage, 3);
            Serial.println();
        }

        _delayWithFlow(interval);
    }
    isMotorStepping = false;

    uint32_t motorTimeMs = millis() - inhaleStartMs;

    Serial.print(F("\n>>>> DELIVERED VOLUME: "));
    Serial.print(steps / STEPS_PER_ML, 1);
    Serial.println(F(" mL <<<<\n"));

    // ===== PHASE 2: INSPIRATORY HOLD =====
    int32_t holdMs = (int32_t)inhaleMs - (int32_t)motorTimeMs;
    if (holdMs > 10) {
        Serial.print(F("[HOLD] Inspiratory plateau: "));
        Serial.print(holdMs); Serial.println(F("ms"));

        uint32_t holdStart = millis();
        while ((millis() - holdStart) < (uint32_t)holdMs) {
            if (_checkAbort()) { breathActive = false; settled = false; return; }
            _delayWithFlow(50000); // 50ms cooperative wait
        }
    } else {
        Serial.println(F("[HOLD] No hold needed (motor time >= inhale time)"));
    }

    // ===== PHASE 3: EXHALE (retract with S-Curve profile) =====
    Serial.println(F("[EXH] Retracting..."));
    digitalWrite(PIN_MOTOR_DIR, MOTOR_DIR_RETRACT);
    delay(5);

    uint32_t exhaleStartMs = millis();
    isMotorStepping = true;
    for (int32_t i = 0; i < steps; i++) {
        if (_checkAbort()) { breathActive = false; isMotorStepping = false; settled = false; return; }

        uint32_t interval = _calcStepInterval(i, steps, exhaleCruiseUs);
        currentStepIntervalUs = interval;
        _fireStep();
        currentPosition--;
        if (currentPosition < 0) currentPosition = 0;

        if (i % TELEMETRY_EVERY_N == 0 || i == steps - 1) {
            Serial.print(F("  EXH Stp=")); Serial.print(i + 1);
            Serial.print(F("/")); Serial.println(steps);
        }

        _delayWithFlow(interval);
    }
    isMotorStepping = false;

    uint32_t exhaleMotorMs = millis() - exhaleStartMs;

    // ===== PHASE 4: EXPIRATORY PAUSE =====
    int32_t pauseMs = (int32_t)exhaleMs - (int32_t)exhaleMotorMs;
    if (pauseMs > 10) {
        Serial.print(F("[PAUSE] Expiratory pause: "));
        Serial.print(pauseMs); Serial.println(F("ms"));
        uint32_t pauseStart = millis();
        while ((millis() - pauseStart) < (uint32_t)pauseMs) {
            if (_checkAbort()) { breathActive = false; settled = false; return; }
            _delayWithFlow(50000); // 50ms cooperative wait
        }
    }

    Serial.println(F("========== BREATH COMPLETE =========="));
    Serial.print(F("Motor compress: ")); Serial.print(motorTimeMs); Serial.println(F("ms"));
    Serial.print(F("Motor retract:  ")); Serial.print(exhaleMotorMs); Serial.println(F("ms"));
    Serial.print(F("Position: ")); Serial.println(currentPosition);
    Serial.println(F("Send C for next breath.\n"));

    breathActive = false;
    settled = false;
}

// =============================================================
// PCV BREATH — Pressure Control Ventilation
//
// The key difference from VCV:
//   INHALE: Motor compresses at controlled speed, but
//           STOPS EARLY if pressure reaches targetPIP_kPa.
//           The motor then holds at whatever position it
//           reached (which may be less than tidalSteps).
//
//   This means:
//   - Volume delivered VARIES based on airway resistance.
//   - Pressure is CONTROLLED (capped at PIP target).
//   - Flow waveform: decelerating (high initial flow → slows
//     as pressure builds → stops at target).
//
// NOTE: Without the Venturi tube and proper airway circuit,
//       PCV won't behave realistically. The flow sensor will
//       only see pressure if there's back-pressure from the
//       Ambu bag being squeezed against something.
// =============================================================
static void _executePCVBreath() {
    breathActive = true;

    uint32_t breathPeriodMs = 60000UL / bpm;
    uint32_t inhaleMs = breathPeriodMs / (1.0f + ieRatio);
    uint32_t exhaleMs = breathPeriodMs - inhaleMs;
    int32_t maxSteps = tidalSteps;

    if (currentPosition + maxSteps > FULL_COMPRESS_STEPS) {
        maxSteps = FULL_COMPRESS_STEPS - currentPosition;
    }
    if (maxSteps <= 0) {
        Serial.println(F("[ERR] No room!"));
        breathActive = false;
        settled = false;
        return;
    }

    Serial.println(F("\n========== PCV BREATH =========="));
    Serial.print(F("BPM=")); Serial.print(bpm);
    Serial.print(F("  I:E=1:")); Serial.print(ieRatio, 1);
    Serial.print(F("  PIP target=")); Serial.print(targetPIP_kPa, 1);
    Serial.println(F("kPa"));
    Serial.print(F("Max Vol=")); Serial.print(targetTidalVolume_mL, 1);
    Serial.print(F(" mL (")); Serial.print(maxSteps); Serial.print(F(" steps)  "));
    Serial.print(F("Inhale=")); Serial.print(inhaleMs);
    Serial.print(F("ms  Exhale=")); Serial.print(exhaleMs);
    Serial.println(F("ms"));
    Serial.println(F("================================"));

    _enableMotor();
    tidalVolume_mL = 0.0f; // Reset volume accumulator
    emaFlow = 0.0f;        // Reset EMA
    breathZeroF = zeroF;   // Start with static zero
    settled = false;       // Mark settling phase as active

    // ===== PHASE 1: INHALE — compress until PIP or max steps =====
    Serial.println(F("[INH] Compressing to target PIP..."));
    digitalWrite(PIN_MOTOR_DIR, MOTOR_DIR_COMPRESS);
    delay(5);

    uint32_t inhaleStartMs = millis();
    int32_t  stepsDelivered = 0;
    bool     pipReached = false;

    for (int32_t i = 0; i < maxSteps; i++) {
        if (_checkAbort()) { breathActive = false; settled = false; return; }

        // --- Dynamic Per-Breath Baseline ---
        if (i == SETTLE_STEPS && !settled) {
            breathZeroF = lastRaw;
            emaFlow = 0.0f;
            tidalVolume_mL = 0.0f;
            settled = true;
            Serial.print(F("  [SETTLE] breathZero=")); Serial.println(breathZeroF, 1);
        }

        if ((millis() - inhaleStartMs) >= inhaleMs) {
            Serial.println(F("  [TIME] Inhale time expired before PIP reached."));
            break;
        }

        float pKpa = lastPressure_kPa;  // Use cached value from flow accumulator
        if (pKpa >= targetPIP_kPa && targetPIP_kPa > 0.0f) {
            pipReached = true;
            Serial.print(F("  [PIP] Target reached at P="));
            Serial.print(pKpa, 2);
            Serial.print(F("kPa  Step="));
            Serial.println(i);
            break;
        }

        uint32_t interval = _calcStepInterval(i, maxSteps, inhaleCruiseUs);
        currentStepIntervalUs = interval;
        
        _fireStep();
        currentPosition++;
        stepsDelivered++;

        if (i % TELEMETRY_EVERY_N == 0 && settled) {
            Serial.print(F("  INH Stp=")); Serial.print(i + 1);
            Serial.print(F("  Flow=")); Serial.print(emaFlow, 1);
            Serial.print(F(" L/min  Vol=")); Serial.print((i + 1) / STEPS_PER_ML, 1);
            Serial.print(F(" mL"));
            Serial.print(F("  Pressure=")); Serial.print(lastPressure_kPa, 3);
            Serial.print(F(" kPa"));
            Serial.print(F("  FlowADC=")); Serial.print((int)lastRawADC);
            Serial.print(F("  FlowVolt=")); Serial.print(lastVoltage, 3);
            Serial.println();
        }

        _delayWithFlow(interval);
    }

    uint32_t motorTimeMs = millis() - inhaleStartMs;

    Serial.print(F("\n>>>> DELIVERED VOLUME: "));
    Serial.print(stepsDelivered / STEPS_PER_ML, 1);
    Serial.println(F(" mL <<<<\n"));

    // ===== PHASE 2: INSPIRATORY HOLD =====
    int32_t holdMs = (int32_t)inhaleMs - (int32_t)motorTimeMs;
    if (holdMs > 10) {
        Serial.print(F("[HOLD] Holding at "));
        Serial.print(stepsDelivered); Serial.print(F(" steps for "));
        Serial.print(holdMs); Serial.println(F("ms"));

        uint32_t holdStart = millis();
        while ((millis() - holdStart) < (uint32_t)holdMs) {
            if (_checkAbort()) { breathActive = false; settled = false; return; }
            _delayWithFlow(50000);
        }
    }

    // ===== PHASE 3: EXHALE — retract however many steps we compressed =====
    Serial.print(F("[EXH] Retracting ")); Serial.print(stepsDelivered);
    Serial.println(F(" steps..."));
    digitalWrite(PIN_MOTOR_DIR, MOTOR_DIR_RETRACT);
    delay(5);

    uint32_t exhaleStartMs = millis();
    for (int32_t i = 0; i < stepsDelivered; i++) {
        if (_checkAbort()) { breathActive = false; settled = false; return; }

        uint32_t interval = _calcStepInterval(i, stepsDelivered, exhaleCruiseUs);
        _fireStep();
        currentPosition--;
        if (currentPosition < 0) currentPosition = 0;

        if (i % TELEMETRY_EVERY_N == 0 || i == stepsDelivered - 1) {
            Serial.print(F("  EXH Stp=")); Serial.print(i + 1);
            Serial.print(F("/")); Serial.println(stepsDelivered);
        }

        _delayWithFlow(interval);
    }

    uint32_t exhaleMotorMs = millis() - exhaleStartMs;

    // ===== PHASE 4: EXPIRATORY PAUSE =====
    int32_t pauseMs = (int32_t)exhaleMs - (int32_t)exhaleMotorMs;
    if (pauseMs > 10) {
        Serial.print(F("[PAUSE] ")); Serial.print(pauseMs);
        Serial.println(F("ms"));
        uint32_t pauseStart = millis();
        while ((millis() - pauseStart) < (uint32_t)pauseMs) {
            if (_checkAbort()) { breathActive = false; settled = false; return; }
            _delayWithFlow(50000);
        }
    }

    Serial.println(F("========== BREATH COMPLETE =========="));
    Serial.print(F("Delivered: ")); Serial.print(stepsDelivered);
    Serial.print(F(" / ")); Serial.print(maxSteps);
    Serial.println(pipReached ? F(" (PIP limited)") : F(" (full volume)"));
    Serial.print(F("Position: ")); Serial.println(currentPosition);
    Serial.println(F("Send C for next breath.\n"));

    breathActive = false;
    settled = false;
}

// =============================================================
// SERIAL INT PARSER (reads digits after command char)
// =============================================================
static int32_t _readSerialInt() {
    delay(30);  // Wait for digits to arrive
    int32_t val = 0;
    bool hasDigit = false;
    while (Serial.available()) {
        char c = Serial.peek();
        if (c >= '0' && c <= '9') {
            Serial.read();
            val = val * 10 + (c - '0');
            hasDigit = true;
        } else {
            break;
        }
    }
    return hasDigit ? val : -1;
}

// =============================================================
// COMMAND HANDLER
// =============================================================
static void _handleCommand(char cmd) {
    if (faultDetected && cmd != 'X' && cmd != 'x' && cmd != '?') {
        Serial.println(F("[ERR] Fault active. Send X to reset."));
        return;
    }
    if (moveActive && cmd != 'X' && cmd != 'x') {
        Serial.println(F("[ERR] Move in progress!"));
        return;
    }

    switch (cmd) {
        // --- Breath ---
        case 'C': case 'c':
            if (vcvMode) {
                _executeVCVBreath();
            } else {
                _executePCVBreath();
            }
            break;

        // --- Mode ---
        case 'V': case 'v':
            vcvMode = true;
            Serial.println(F("[MODE] VCV — fixed volume, variable pressure"));
            break;
        case 'P': case 'p':
            vcvMode = false;
            Serial.println(F("[MODE] PCV — fixed pressure, variable volume"));
            break;

        // --- Manual forward ---
        case 'F': case 'f': {
            int32_t numSteps = strokeSteps;
            if (currentPosition + numSteps > FULL_COMPRESS_STEPS) {
                numSteps = FULL_COMPRESS_STEPS - currentPosition;
            }
            if (numSteps <= 0) { Serial.println(F("[ERR] At max!")); break; }
            _enableMotor();
            digitalWrite(PIN_MOTOR_DIR, MOTOR_DIR_COMPRESS);  delay(5);
            moveDirection = MOTOR_DIR_COMPRESS;
            moveTarget = numSteps;  moveCompleted = 0;
            lastStepUs = micros();  moveActive = true;
            Serial.print(F("[FWD] ")); Serial.print(numSteps); Serial.println(F(" steps"));
            break;
        }

        // --- Manual retract ---
        case 'b': {
            // lowercase 'b' alone = retract to home
            if (currentPosition <= 0) { Serial.println(F("[ERR] At home!")); break; }
            _enableMotor();
            digitalWrite(PIN_MOTOR_DIR, MOTOR_DIR_RETRACT);  delay(5);
            moveDirection = MOTOR_DIR_RETRACT;
            moveTarget = currentPosition;  moveCompleted = 0;
            lastStepUs = micros();  moveActive = true;
            Serial.print(F("[REV] ")); Serial.print(currentPosition); Serial.println(F(" steps"));
            break;
        }

        // --- BPM set (uppercase B + digits) ---
        case 'B': {
            int32_t val = _readSerialInt();
            if (val >= 10 && val <= 30) {
                bpm = val;
                updateMotorKinematics();
                Serial.print(F("[SET] BPM=")); Serial.println(bpm);
            } else if (val == -1) {
                // Just 'B' with no digits — treat as retract
                if (currentPosition <= 0) { Serial.println(F("[ERR] At home!")); break; }
                _enableMotor();
                digitalWrite(PIN_MOTOR_DIR, MOTOR_DIR_RETRACT); delay(5);
                moveDirection = MOTOR_DIR_RETRACT;
                moveTarget = currentPosition; moveCompleted = 0;
                lastStepUs = micros(); moveActive = true;
                Serial.print(F("[REV] ")); Serial.print(currentPosition); Serial.println(F(" steps"));
            } else {
                Serial.println(F("[ERR] BPM range: 10-30"));
            }
            break;
        }

        // --- I:E Ratio ---
        case 'R': case 'r': {
            int32_t val = _readSerialInt();
            if (val >= 10 && val <= 40) {
                ieRatio = val / 10.0f;
                updateMotorKinematics();
                Serial.print(F("[SET] I:E=1:")); Serial.println(ieRatio, 1);
            } else {
                Serial.println(F("[ERR] I:E range: R10-R40 (1:1.0 to 1:4.0)"));
            }
            break;
        }

        // --- Tidal Volume ---
        case 'T': case 't': {
            int32_t val = _readSerialInt();
            if (val >= 50 && val <= 750) {
                targetTidalVolume_mL = val;
                updateMotorKinematics();
                Serial.print(F("[SET] TV=")); Serial.print(targetTidalVolume_mL, 1);
                Serial.print(F(" mL (")); Serial.print(tidalSteps);
                Serial.println(F(" steps)"));
            } else {
                Serial.println(F("[ERR] TV range: 50-750 mL"));
            }
            break;
        }

        // --- PIP Target ---
        case 'I': case 'i': {
            int32_t val = _readSerialInt();
            if (val >= 5 && val <= 50) {
                targetPIP_kPa = val / 10.0f;
                Serial.print(F("[SET] PIP=")); Serial.print(targetPIP_kPa, 1);
                Serial.println(F("kPa"));
            } else {
                Serial.println(F("[ERR] PIP range: I5-I50 (0.5-5.0 kPa)"));
            }
            break;
        }

        // --- Stroke adjust ---
        case '+':
            strokeSteps = min(strokeSteps + 50, (int32_t)FULL_COMPRESS_STEPS);
            tidalSteps = strokeSteps;
            _printStrokeSet();
            break;
        case '-':
            strokeSteps = max(strokeSteps - 50, (int32_t)50);
            tidalSteps = strokeSteps;
            _printStrokeSet();
            break;

        // --- Speed adjust ---
        case '<': case ',':
            inhaleCruiseUs = max(inhaleCruiseUs - 50, (uint32_t)MIN_INTERVAL_US);
            Serial.print(F("[SET] Inhale Cruise: ")); Serial.print(inhaleCruiseUs);
            Serial.print(F("us (")); Serial.print(1000000UL / inhaleCruiseUs);
            Serial.println(F(" stp/s)"));
            break;
        case '>': case '.':
            inhaleCruiseUs = min(inhaleCruiseUs + 50, (uint32_t)MAX_INTERVAL_US);
            Serial.print(F("[SET] Inhale Cruise: ")); Serial.print(inhaleCruiseUs);
            Serial.print(F("us (")); Serial.print(1000000UL / inhaleCruiseUs);
            Serial.println(F(" stp/s)"));
            break;

        case 'H': case 'h':
            _homeSequence();
            break;

        case 'A': case 'a':
            almEnabled = !almEnabled;
            Serial.print(F("[SET] ALM+: "));
            Serial.println(almEnabled ? F("ON") : F("OFF"));
            break;

        case 'D': case 'd':
            _disableMotor();
            Serial.println(F("[OFF] Motor disabled."));
            break;

        case '1': strokeSteps = FULL_COMPRESS_STEPS / 10;     tidalSteps = strokeSteps; _printStrokeSet(); break; // 10%
        case '2': strokeSteps = FULL_COMPRESS_STEPS / 4;      tidalSteps = strokeSteps; _printStrokeSet(); break; // 25%
        case '3': strokeSteps = FULL_COMPRESS_STEPS / 2;      tidalSteps = strokeSteps; _printStrokeSet(); break; // 50%
        case '4': strokeSteps = (FULL_COMPRESS_STEPS * 3) / 4; tidalSteps = strokeSteps; _printStrokeSet(); break; // 75%
        case '5': strokeSteps = FULL_COMPRESS_STEPS;           tidalSteps = strokeSteps; _printStrokeSet(); break; // 100%

        case '?': _printStatus(); break;

        case 'X': case 'x':
            _emergencyStop();
            faultDetected = false;  // Allow recovery
            currentPosition = 0;    // Reset position tracking
            Serial.println(F("[STOP] Emergency stop. Position reset to 0."));
            break;

        case '\n': case '\r': break;
        default: break;
    }
}

// =============================================================
// MOTOR HELPERS
// =============================================================
static void _enableMotor() {
    digitalWrite(PIN_MOTOR_ENA, LOW);
    motorEnabled = true;
    delay(10);
}

static void _disableMotor() {
    digitalWrite(PIN_MOTOR_ENA, HIGH);
    motorEnabled = false;
    digitalWrite(PIN_LED_YELLOW, LOW);
}

static void _emergencyStop() {
    moveActive = false;
    breathActive = false;
    _disableMotor();
    digitalWrite(PIN_LED_RED, HIGH);
    _buzzError();
    delay(200);
    digitalWrite(PIN_LED_RED, LOW);
}

// =============================================================
// HOMING
// =============================================================
static void _homeSequence() {
    int hallVal = analogRead(PIN_HALL_SENSOR);
    if (hallVal < HALL_TRIGGER_THRESHOLD) {
        currentPosition = 0;
        isHomed = true;
        Serial.println(F("[HOME] Hall triggered. Position = 0."));
        return;
    }

    Serial.println(F("[HOME] Place magnet near Hall sensor..."));
    Serial.println(F("       Or press X to abort."));

    _enableMotor();
    digitalWrite(PIN_MOTOR_DIR, MOTOR_DIR_RETRACT);
    delay(5);

    for (int i = 0; i < FULL_COMPRESS_STEPS + 400; i++) {
        hallVal = analogRead(PIN_HALL_SENSOR);
        if (hallVal < HALL_TRIGGER_THRESHOLD) {
            currentPosition = 0;
            isHomed = true;
            Serial.print(F("[HOME] Done at step ")); Serial.println(i);
            _disableMotor();
            return;
        }
        _fireStep();
        delayMicroseconds(1200);

        if (Serial.available()) {
            char c = Serial.peek();
            if (c == 'X' || c == 'x') {
                Serial.read();
                Serial.println(F("[HOME] Aborted."));
                _disableMotor();
                return;
            }
        }
    }

    Serial.println(F("[HOME] WARNING: Hall not found!"));
    _disableMotor();
    _buzzWarning();
}

// =============================================================
// UI HELPERS
// =============================================================
static void _printStrokeSet() {
    targetTidalVolume_mL = strokeSteps / STEPS_PER_ML;
    updateMotorKinematics();
    Serial.print(F("[SET] Target Vol = ")); Serial.print(targetTidalVolume_mL, 1);
    Serial.print(F(" mL / 750.0 mL ("));
    Serial.print(strokeSteps); Serial.println(F(" steps)"));
}

static void _printStatus() {
    uint32_t breathMs = 60000UL / bpm;
    uint32_t inhMs = breathMs / (1.0f + ieRatio);
    uint32_t exhMs = breathMs - inhMs;

    Serial.println(F("\n--- System Status ---"));
    Serial.print(F("  Mode     : ")); Serial.println(vcvMode ? F("VCV") : F("PCV"));
    Serial.print(F("  BPM      : ")); Serial.println(bpm);
    Serial.print(F("  I:E      : 1:")); Serial.println(ieRatio, 1);
    Serial.print(F("  T_inh    : ")); Serial.print(inhMs); Serial.println(F("ms"));
    Serial.print(F("  T_exh    : ")); Serial.print(exhMs); Serial.println(F("ms"));
    Serial.print(F("  Tidal Vol: ")); Serial.print(targetTidalVolume_mL, 1);
    Serial.print(F(" mL (")); Serial.print(tidalSteps);
    Serial.print(F(" / ")); Serial.print(FULL_COMPRESS_STEPS);
    Serial.println(F(" steps)"));
    Serial.print(F("  PIP Tgt  : ")); Serial.print(targetPIP_kPa, 1);
    Serial.println(F(" kPa"));
    Serial.print(F("  Inhale Cr: ")); Serial.print(inhaleCruiseUs);
    Serial.print(F("us (")); Serial.print(1000000UL / inhaleCruiseUs);
    Serial.println(F(" stp/s)"));
    
    float rpm = (1000000.0f / inhaleCruiseUs / PULSES_PER_REV) * 60.0f;
    Serial.print(F("  RPM      : ")); Serial.println(rpm, 1);

    Serial.print(F("  Position : ")); Serial.print(currentPosition);
    Serial.print(F(" / ")); Serial.println(FULL_COMPRESS_STEPS);
    Serial.print(F("  Homed    : ")); Serial.println(isHomed ? F("YES") : F("NO"));
    Serial.print(F("  Motor    : ")); Serial.println(motorEnabled ? F("ON") : F("OFF"));
    Serial.print(F("  ALM+     : ")); Serial.println(almEnabled ? F("ON") : F("OFF"));

    float pNow = _readPressureKpa();
    Serial.print(F("  Pressure : ")); Serial.print(pNow, 3);
    Serial.print(F(" kPa  ("));
    Serial.print(pNow * 10.1972f, 1);  // Convert kPa to cmH2O
    Serial.println(F(" cmH2O)"));
    Serial.print(F("  BMP280   : Amb="));
    Serial.print(bmpAmbientOk ? F("OK") : F("N/A"));
    Serial.print(F("  Air="));
    Serial.println(bmpAirwayOk ? F("OK") : F("N/A"));

    int hallVal = analogRead(PIN_HALL_SENSOR);
    Serial.print(F("  Hall     : ")); Serial.print(hallVal);
    Serial.println(hallVal < HALL_TRIGGER_THRESHOLD ? F(" (HOME)") : F(" (clear)"));
    Serial.println(F("---------------------\n"));
}

static void _buzzWarning() {
    for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_BUZZER, HIGH); delay(100);
        digitalWrite(PIN_BUZZER, LOW);  delay(100);
    }
}

static void _buzzError() {
    for (int i = 0; i < 5; i++) {
        digitalWrite(PIN_BUZZER, HIGH); delay(200);
        digitalWrite(PIN_BUZZER, LOW);  delay(100);
    }
}
