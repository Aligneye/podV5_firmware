#include "calibration.h"
#include "button.h"
#include "motor.h"
#include "therapy.h"
#include "training.h"
#include <math.h>
#include <string.h>

extern RTTStream rtt;

// ── Mirror correctv1deviceEsp32c3-main/src/calibration.cpp + vibration timings ──
enum CalibState { CALIB_IDLE, CALIB_HOLD };
static CalibState calibState = CALIB_IDLE;

static constexpr uint32_t CALIB_GET_READY_MS       = 3000UL;
static constexpr uint32_t CALIB_HOLD_MS          = 5000UL;
static constexpr uint32_t CALIB_TOTAL_MS         = CALIB_GET_READY_MS + CALIB_HOLD_MS;
static constexpr uint32_t CALIB_RESULT_BROADCAST_MS = 4000UL;
static constexpr uint32_t kSafetyTimeoutMs       = 10000UL;
static constexpr uint32_t kSampleIntervalMs    = 50UL;

/** Start pulse — same role as ESP32 `playCalibrationFeedback(true)` initial buzz. */
static constexpr uint32_t kCalibMotorPulseMs = 150UL;
/** GET_READY tick — same as ESP32 `playButtonFeedback()` (~30 ms). */
static constexpr uint32_t kCalibTickPulseMs = 30UL;
/** Failure / timeout — ESP32 used `startVibration(255); delay(2000);`. */
static constexpr uint32_t kCalibFailMotorMs = 2000UL;
/** Success — ESP32 used `startVibration(255); delay(1000);`. */
static constexpr uint32_t kCalibSuccessMotorMs = 1000UL;

static volatile bool pendingStart  = false;
static volatile bool pendingCancel = false;

static unsigned long stabilityStartTime = 0;
static unsigned long lastBeepTime       = 0;
static float         lastCalibX       = 0;
static float         lastCalibY       = 0;
static float         lastCalibZ       = 0;

static float         sumY        = 0;
static float         sumZ        = 0;
static int           sampleCount = 0;
static unsigned long s_lastSampleTime = 0;

static char          lastCalibrationResult[16] = "";
static unsigned long calibrationResultSetAt   = 0;

static unsigned long s_calibPulseEndMs  = 0;
static unsigned long s_tickPulseEndMs   = 0;
static unsigned long s_failVibEndMs     = 0;
static unsigned long s_successPulseEndMs = 0;

static void goToTrainingMode() {
    deviceOn = true;
    setDeviceMode(MODE_TRAINING);
}

/** ESP32 playCalibrationFeedback: MAX ~150 ms */
static void playCalibrationMotorPulse() {
    const unsigned long t = millis();
    s_calibPulseEndMs = t + kCalibMotorPulseMs;
    motorSetDuty(VIB_INTENSITY_MAX);
}

/** ESP32 playButtonFeedback: LOW ~30 ms */
static void playButtonFeedbackMotor() {
    s_tickPulseEndMs = millis() + kCalibTickPulseMs;
    motorSetDuty(VIB_INTENSITY_LOW);
}

static void applyIdleMotorFeedback(uint32_t now) {
    if (s_failVibEndMs != 0u) {
        if ((int32_t)(now - s_failVibEndMs) < 0) {
            motorSetDuty(VIB_INTENSITY_HIGH);
        } else {
            motorSetDuty(0);
            s_failVibEndMs = 0;
        }
        return;
    }
    if (s_successPulseEndMs != 0u) {
        if ((int32_t)(now - s_successPulseEndMs) < 0) {
            motorSetDuty(VIB_INTENSITY_MAX);
        } else {
            motorSetDuty(0);
            s_successPulseEndMs = 0;
        }
    }
}

/** During CALIB_HOLD: calibration pulse, then tick pulses, else off in GET_READY; off in HOLD. */
static void applyHoldMotor(uint32_t now, unsigned long elapsed) {
    if (now < s_calibPulseEndMs) {
        motorSetDuty(VIB_INTENSITY_MAX);
        return;
    }
    if (s_tickPulseEndMs != 0u && now < s_tickPulseEndMs) {
        motorSetDuty(VIB_INTENSITY_LOW);
        return;
    }
    if (elapsed >= CALIB_GET_READY_MS) {
        motorSetDuty(0);
        return;
    }
    motorSetDuty(0);
}

void initCalibration() {
    calibState   = CALIB_IDLE;
    pendingStart = false;
    pendingCancel = false;
    motorSetDuty(0);
    s_calibPulseEndMs   = 0;
    s_tickPulseEndMs    = 0;
    s_failVibEndMs      = 0;
    s_successPulseEndMs = 0;
    s_lastSampleTime    = 0;
}

const char* getCalibrationResult() {
    if (isCalibrating()) {
        return "";
    }
    if (lastCalibrationResult[0] == '\0') {
        return "";
    }
    const unsigned long now = millis();
    if ((now - calibrationResultSetAt) > CALIB_RESULT_BROADCAST_MS) {
        lastCalibrationResult[0] = '\0';
        return "";
    }
    return lastCalibrationResult;
}

bool isCalibrating() {
    return calibState != CALIB_IDLE;
}

uint32_t getCalibrationElapsedMs() {
    if (!isCalibrating()) {
        return 0;
    }
    return (uint32_t)(millis() - stabilityStartTime);
}

uint32_t getCalibrationTotalMs() {
    return CALIB_TOTAL_MS;
}

const char* getCalibrationPhase() {
    if (!isCalibrating()) {
        return "IDLE";
    }
    const unsigned long elapsed = getCalibrationElapsedMs();
    if (elapsed < CALIB_GET_READY_MS) {
        return "GET_READY";
    }
    return "HOLD_STILL";
}

void requestCalibrationStart() {
    pendingStart = true;
}

void requestCalibrationCancel() {
    pendingCancel = true;
}

void startCalibration() {
    if (calibState != CALIB_IDLE) {
        return;
    }
    if (!trainingSampleAccelForCalibration()) {
        rtt.println("Calibration: cannot start (no accelerometer)");
        return;
    }

    lastCalibrationResult[0] = '\0';
    calibrationResultSetAt = 0;

    deviceOn = true;
    wakePostureSensor();
    if (therapyIsRunning()) {
        therapyStop(false);
    }

    playCalibrationMotorPulse();

    calibState         = CALIB_HOLD;
    stabilityStartTime = millis();
    lastBeepTime       = millis();

    sumY        = 0;
    sumZ        = 0;
    sampleCount = 0;

    lastCalibX = rawX;
    lastCalibY = rawY;
    lastCalibZ = rawZ;

    s_lastSampleTime = 0;

    rtt.println("CALIBRATION: START");
}

void cancelCalibration() {
    if (calibState == CALIB_IDLE) {
        return;
    }
    rtt.println("CALIBRATION: CANCELLED");
    calibState = CALIB_IDLE;
    motorSetDuty(0);
    s_calibPulseEndMs   = 0;
    s_tickPulseEndMs    = 0;
    s_failVibEndMs      = 0;
    s_successPulseEndMs = 0;
    goToTrainingMode();
}

void handleCalibration() {
    const unsigned long currentMillis = millis();

    if (pendingCancel) {
        pendingCancel = false;
        cancelCalibration();
        applyIdleMotorFeedback(currentMillis);
        return;
    }
    if (pendingStart && calibState == CALIB_IDLE) {
        pendingStart = false;
        startCalibration();
        return;
    }

    if (calibState == CALIB_IDLE) {
        applyIdleMotorFeedback(currentMillis);
        return;
    }

    if (calibState != CALIB_HOLD) {
        return;
    }

    const unsigned long elapsed = currentMillis - stabilityStartTime;

    if (elapsed > kSafetyTimeoutMs) {
        rtt.println("CALIB: TIMEOUT - Failed");
        calibState = CALIB_IDLE;
        strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
        lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
        calibrationResultSetAt = millis();
        s_failVibEndMs = millis() + kCalibFailMotorMs;
        motorSetDuty(VIB_INTENSITY_HIGH);
        goToTrainingMode();
        return;
    }

    if (elapsed < CALIB_GET_READY_MS) {
        if (currentMillis - lastBeepTime >= 1000UL) {
            lastBeepTime = currentMillis;
            playButtonFeedbackMotor();
        }

        if (trainingSampleAccelForCalibration()) {
            lastCalibX = rawX;
            lastCalibY = rawY;
            lastCalibZ = rawZ;
        }
        sumY        = 0;
        sumZ        = 0;
        sampleCount = 0;

        applyHoldMotor(currentMillis, elapsed);
        return;
    }

    if (currentMillis - s_lastSampleTime >= kSampleIntervalMs) {
        s_lastSampleTime = currentMillis;

        if (!trainingSampleAccelForCalibration()) {
            calibState = CALIB_IDLE;
            strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
            lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
            calibrationResultSetAt = millis();
            s_failVibEndMs = millis() + kCalibFailMotorMs;
            motorSetDuty(VIB_INTENSITY_HIGH);
            goToTrainingMode();
            return;
        }

        const float dy = fabsf(rawY - lastCalibY);
        const float dz = fabsf(rawZ - lastCalibZ);
        const float movement = dy + dz;

        lastCalibX = rawX;
        lastCalibY = rawY;
        lastCalibZ = rawZ;

        if (movement > CALIB_THRESHOLD) {
            rtt.println("CALIB: BAD MOVEMENT - Failed");
            calibState = CALIB_IDLE;
            strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
            lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
            calibrationResultSetAt = millis();
            s_failVibEndMs = millis() + kCalibFailMotorMs;
            motorSetDuty(VIB_INTENSITY_HIGH);
            goToTrainingMode();
            return;
        }

        sumY += rawY;
        sumZ += rawZ;
        sampleCount++;
    }

    if (elapsed > CALIB_TOTAL_MS) {
        if (sampleCount < (int)CALIB_MIN_SAMPLES) {
            rtt.println("CALIB: Too few samples - Failed");
            calibState = CALIB_IDLE;
            strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
            lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
            calibrationResultSetAt = millis();
            s_failVibEndMs = millis() + kCalibFailMotorMs;
            motorSetDuty(VIB_INTENSITY_HIGH);
            goToTrainingMode();
            return;
        }

        const float avgY = sumY / (float)sampleCount;
        const float avgZ = sumZ / (float)sampleCount;
        setPostureOrigin(avgY, avgZ);

        rtt.print("CALIBRATION: DONE. Samples:");
        rtt.print(sampleCount);
        rtt.print(" AvgY=");
        rtt.print(avgY, 2);
        rtt.print(" AvgZ=");
        rtt.println(avgZ, 2);

        calibState = CALIB_IDLE;
        strncpy(lastCalibrationResult, "complete", sizeof(lastCalibrationResult) - 1);
        lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
        calibrationResultSetAt = millis();

        s_successPulseEndMs = millis() + kCalibSuccessMotorMs;
        motorSetDuty(VIB_INTENSITY_MAX);

        goToTrainingMode();
        return;
    }

    applyHoldMotor(currentMillis, elapsed);
}

void calibrationSetup() {
    initCalibration();
}

void calibrationLoop() {
    handleCalibration();
}

void calibrationRequestStart() {
    requestCalibrationStart();
}

void calibrationRequestCancel() {
    requestCalibrationCancel();
}

void calibrationStart() {
    requestCalibrationStart();
}

void calibrationStop() {
    requestCalibrationCancel();
}

bool calibrationIsActive() {
    return isCalibrating();
}

bool calibrationMotorActive() {
    if (isCalibrating()) {
        return true;
    }
    const unsigned long now = millis();
    if (s_failVibEndMs != 0u && (int32_t)(now - s_failVibEndMs) < 0) {
        return true;
    }
    if (s_successPulseEndMs != 0u && (int32_t)(now - s_successPulseEndMs) < 0) {
        return true;
    }
    return false;
}
