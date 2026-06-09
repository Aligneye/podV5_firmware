#include "calibration.h"
#include "button.h"
#include "motor.h"
#include "therapy.h"
#include "training.h"
#include "storage.h"
#include <math.h>
#include <string.h>

extern RTTStream rtt;

// ── State Machine Timings ───────────────────────────────────────────────────
enum CalibState { CALIB_STATE_IDLE, CALIB_STATE_GET_READY, CALIB_STATE_HOLD_STILL };
static CalibState calibState = CALIB_STATE_IDLE;

static constexpr uint32_t CALIB_GET_READY_MS       = 0UL;
static constexpr uint32_t CALIB_HOLD_MS            = 5000UL;
static constexpr uint32_t CALIB_TOTAL_MS           = CALIB_GET_READY_MS + CALIB_HOLD_MS;
static constexpr uint32_t CALIB_RESULT_BROADCAST_MS = 4000UL;
static constexpr uint32_t kSafetyTimeoutMs         = 10000UL;
static constexpr uint32_t kSampleIntervalMs        = 50UL;
static constexpr int      MIN_VALID_SAMPLES        = 70;

static volatile bool pendingStart  = false;
static volatile bool pendingCancel = false;

static unsigned long stabilityStartTime = 0;
static unsigned long lastBeepTime       = 0;
static unsigned long lastHoldPrintMs     = 0;
static float         lastCalibX         = 0;
static float         lastCalibY         = 0;
static float         lastCalibZ         = 0;

static float         sumX               = 0;
static float         sumY               = 0;
static float         sumZ               = 0;
static int           sampleCount        = 0;
static int           totalSamples       = 0;
static unsigned long s_lastSampleTime   = 0;

static float         samplesX[100];
static float         samplesY[100];
static float         samplesZ[100];

static char          lastCalibrationResult[16] = "";
static unsigned long calibrationResultSetAt    = 0;

static unsigned long s_failVibEndMs      = 0;
static unsigned long s_successPulseEndMs = 0;

// Temporary buffer for successful calibration before profile naming
static float s_lastCalibratedX = 0.0f;
static float s_lastCalibratedY = 0.0f;
static float s_lastCalibratedZ = 0.0f;
static bool  s_lastCalibrationValid = false;

// ── Helpers ─────────────────────────────────────────────────────────────────
static void goToTrainingMode() {
    deviceOn = true;
    setDeviceMode(MODE_TRAINING);
}

static void calibrationFail(const char* reason) {
    calibState = CALIB_STATE_IDLE;
    strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
    lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
    calibrationResultSetAt = millis();
    
    // Failure pulse: 500ms duration, 150 duty cycle
    s_failVibEndMs = millis() + 500UL;
    motorOverrideDuty(150, 500);

    s_lastCalibrationValid = false;

    // Exact console log format: "CALIB: BAD MOVEMENT - Failed" or generic "CALIBRATION FAILED: <reason>"
    if (strcmp(reason, "Bad movement") == 0 || strcmp(reason, "Too much movement") == 0) {
        rtt.println("CALIB: BAD MOVEMENT - Failed");
    } else {
        rtt.printf("CALIBRATION FAILED: %s\n", reason);
    }

    goToTrainingMode();
}

static void calibrationSuccess(float avgX, float avgY, float avgZ) {
    calibState = CALIB_STATE_IDLE;
    strncpy(lastCalibrationResult, "complete", sizeof(lastCalibrationResult) - 1);
    lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
    calibrationResultSetAt = millis();
    
    // Success pulse: 125ms duration, 150 duty cycle
    s_successPulseEndMs = millis() + 125UL;
    motorOverrideDuty(150, 125);

    s_lastCalibratedX = avgX;
    s_lastCalibratedY = avgY;
    s_lastCalibratedZ = avgZ;
    s_lastCalibrationValid = true;

    // Log exact formats
    rtt.println("CALIBRATION: DONE");

    // If empty profile database, auto-initialize as Default Vertical
    if (getProfileCount() == 0) {
        addOrUpdateProfile0(avgX, avgY, avgZ);
    }

    goToTrainingMode();
}

// ── Temporary Calibration Results retrieval ─────────────────────────────────
float getLastCalibratedX() { return s_lastCalibratedX; }
float getLastCalibratedY() { return s_lastCalibratedY; }
float getLastCalibratedZ() { return s_lastCalibratedZ; }
bool isLastCalibrationValid() { return s_lastCalibrationValid; }

// ── Lifecycle ───────────────────────────────────────────────────────────────
void initCalibration() {
    calibState   = CALIB_STATE_IDLE;
    pendingStart = false;
    pendingCancel = false;
    motorSetDuty(0);
    s_failVibEndMs      = 0;
    s_successPulseEndMs = 0;
    s_lastSampleTime    = 0;
    s_lastCalibrationValid = false;
    totalSamples = 0;

    initProfiles();
}

void handleCalibration() {
    const unsigned long currentMillis = millis();

    if (pendingCancel) {
        pendingCancel = false;
        cancelCalibration();
        return;
    }
    if (pendingStart && calibState == CALIB_STATE_IDLE) {
        pendingStart = false;
        startCalibration();
        return;
    }

    if (calibState == CALIB_STATE_IDLE) {
        return;
    }

    const unsigned long elapsed = currentMillis - stabilityStartTime;

    static unsigned long lastDebugPrintMs = 0;
    if (currentMillis - lastDebugPrintMs >= 1000UL) {
        lastDebugPrintMs = currentMillis;
        rtt.printf("DEBUG: calibState=%d, elapsed=%lu, totalSamples=%d, sampleCount=%d, dtSample=%lu\n",
                   (int)calibState, elapsed, totalSamples, sampleCount, currentMillis - s_lastSampleTime);
    }

    if (elapsed > kSafetyTimeoutMs) {
        calibrationFail("Timeout");
        return;
    }

    if (calibState == CALIB_STATE_HOLD_STILL) {
        // Detailed RTT prints for HOLD STILL countdown
        if (currentMillis - lastHoldPrintMs >= 1000UL) {
            lastHoldPrintMs = currentMillis;
            uint32_t msLeft = (CALIB_TOTAL_MS > elapsed) ? (CALIB_TOTAL_MS - elapsed) : 0;
            int secondsLeft = (msLeft + 500UL) / 1000UL;
            if (secondsLeft > 0) {
                rtt.printf("CALIBRATION: HOLD STILL - %d sec\n", secondsLeft);
            }
        }

        if (currentMillis - s_lastSampleTime >= kSampleIntervalMs) {
            s_lastSampleTime = currentMillis;

            if (!trainingSampleAccelForCalibration()) {
                calibrationFail("Lost accelerometer");
                return;
            }

            if (totalSamples < 100) {
                samplesX[totalSamples] = rawX;
                samplesY[totalSamples] = rawY;
                samplesZ[totalSamples] = rawZ;
                totalSamples++;

                // Print every sample values immediately
                rtt.printf("CALIB: Sample #%d - raw[%s, %s, %s]\n",
                           totalSamples, String(rawX, 2).c_str(), String(rawY, 2).c_str(), String(rawZ, 2).c_str());
            }
        }

        if (elapsed >= CALIB_TOTAL_MS) {
            if (totalSamples == 0) {
                calibrationFail("Too few samples");
                return;
            }

            // Pass 1: Calculate Mean of all collected samples
            float sumAllX = 0, sumAllY = 0, sumAllZ = 0;
            for (int i = 0; i < totalSamples; i++) {
                sumAllX += samplesX[i];
                sumAllY += samplesY[i];
                sumAllZ += samplesZ[i];
            }
            float meanX = sumAllX / (float)totalSamples;
            float meanY = sumAllY / (float)totalSamples;
            float meanZ = sumAllZ / (float)totalSamples;

            // Pass 2: Calculate Variance and Standard Deviation
            float varX = 0, varY = 0, varZ = 0;
            for (int i = 0; i < totalSamples; i++) {
                float dx = samplesX[i] - meanX;
                float dy = samplesY[i] - meanY;
                float dz = samplesZ[i] - meanZ;
                varX += dx * dx;
                varY += dy * dy;
                varZ += dz * dz;
            }
            float stdDevX = sqrtf(varX / (float)totalSamples);
            float stdDevY = sqrtf(varY / (float)totalSamples);
            float stdDevZ = sqrtf(varZ / (float)totalSamples);

            // Log statistics
            rtt.printf("CALIB STATS: Mean[%s, %s, %s], StdDev[%s, %s, %s]\n",
                       String(meanX, 2).c_str(), String(meanY, 2).c_str(), String(meanZ, 2).c_str(),
                       String(stdDevX, 2).c_str(), String(stdDevY, 2).c_str(), String(stdDevZ, 2).c_str());

            // Safety limit: if standard deviation is too high, posture is too unstable
            if (stdDevX > 1.0f || stdDevY > 1.0f || stdDevZ > 1.0f) {
                calibrationFail("Too much movement");
                return;
            }

            // Pass 3: Reject outliers (mean +/- 2*sigma) and compute final average
            float finalSumX = 0, finalSumY = 0, finalSumZ = 0;
            int validCount = 0;

            for (int i = 0; i < totalSamples; i++) {
                // If stdDev is near zero, accept all (protect from floating point edge case)
                bool okX = (stdDevX < 0.01f) || (fabsf(samplesX[i] - meanX) <= 2.0f * stdDevX);
                bool okY = (stdDevY < 0.01f) || (fabsf(samplesY[i] - meanY) <= 2.0f * stdDevY);
                bool okZ = (stdDevZ < 0.01f) || (fabsf(samplesZ[i] - meanZ) <= 2.0f * stdDevZ);

                if (okX && okY && okZ) {
                    finalSumX += samplesX[i];
                    finalSumY += samplesY[i];
                    finalSumZ += samplesZ[i];
                    validCount++;
                } else {
                    rtt.printf("CALIB: Outlier Sample #%d rejected - raw[%s, %s, %s]\n",
                               i + 1, String(samplesX[i], 2).c_str(), String(samplesY[i], 2).c_str(), String(samplesZ[i], 2).c_str());
                }
            }

            rtt.printf("CALIB RESULTS: Valid samples=%d/%d\n", validCount, totalSamples);

            if (validCount < MIN_VALID_SAMPLES) {
                calibrationFail("Too much movement");
                return;
            }

            const float avgX = finalSumX / (float)validCount;
            const float avgY = finalSumY / (float)validCount;
            const float avgZ = finalSumZ / (float)validCount;
            calibrationSuccess(avgX, avgY, avgZ);
            return;
        }
    }
}

void requestCalibrationStart() {
    pendingStart = true;
}

void requestCalibrationCancel() {
    pendingCancel = true;
    cancelCalibration();
}

void startCalibration() {
    if (calibState != CALIB_STATE_IDLE) {
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

    // Start calibration with start haptic pulse (150 duty for 150ms)
    motorOverrideDuty(150, 150);

    calibState         = CALIB_STATE_HOLD_STILL;
    stabilityStartTime = millis();
    lastBeepTime       = millis();
    lastHoldPrintMs    = millis();

    sumX        = 0;
    sumY        = 0;
    sumZ        = 0;
    sampleCount = 0;
    totalSamples = 0;

    lastCalibX = rawX;
    lastCalibY = rawY;
    lastCalibZ = rawZ;

    s_lastSampleTime = millis();

    rtt.println("CALIBRATION: START");
    rtt.println("CALIBRATION: HOLD STILL - 5 sec");
}

void cancelCalibration() {
    if (calibState == CALIB_STATE_IDLE) {
        return;
    }
    rtt.println("CALIBRATION: CANCELLED");
    calibState = CALIB_STATE_IDLE;
    motorSetDuty(0);
    s_failVibEndMs      = 0;
    s_successPulseEndMs = 0;
    s_lastCalibrationValid = false;
    goToTrainingMode();
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
    return calibState != CALIB_STATE_IDLE;
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
    if (calibState == CALIB_STATE_GET_READY) {
        return "GET_READY";
    }
    return "HOLD_STILL";
}

// ── Legacy Aliases ──────────────────────────────────────────────────────────
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
