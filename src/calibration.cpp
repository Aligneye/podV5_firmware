#include "calibration.h"
#include "bluetooth.h"
#include "button.h"
#include "motor.h"
#include "therapy.h"
#include "training.h"
#include "storage.h"
#include "monitor_log.h"
#include <math.h>
#include <string.h>

extern RTTStream rtt;

// ── State Machine Timings ───────────────────────────────────────────────────
enum CalibState { CALIB_STATE_IDLE, CALIB_STATE_GET_READY, CALIB_STATE_HOLD_STILL };
static CalibState calibState = CALIB_STATE_IDLE;

static constexpr uint32_t CALIB_GET_READY_MS       = 3000UL;
static constexpr uint32_t CALIB_HOLD_MS            = 5000UL;
static constexpr uint32_t CALIB_TOTAL_MS           = CALIB_GET_READY_MS + CALIB_HOLD_MS;
static constexpr uint32_t CALIB_RESULT_BROADCAST_MS = 4000UL;
static constexpr uint32_t kSafetyTimeoutMs         = CALIB_TOTAL_MS + 2000UL;
static constexpr uint32_t kSampleIntervalMs        = 50UL;
static constexpr int      kMaxCalibrationSamples   = 200;
static constexpr int      MIN_VALID_SAMPLES        = 70;
static constexpr int      kEarlyFailMinSamples     = 40;
static constexpr float    kFinalStdDevLimit        = 1.0f;
static constexpr float    kEarlyFailStdDevLimit    = 1.75f;

static volatile bool pendingStart  = false;
static volatile bool pendingCancel = false;

static unsigned long stabilityStartTime = 0;
static unsigned long lastHoldPrintMs     = 0;

static int           totalSamples       = 0;
static unsigned long s_lastSampleTime   = 0;

static float         samplesX[kMaxCalibrationSamples];
static float         samplesY[kMaxCalibrationSamples];
static float         samplesZ[kMaxCalibrationSamples];

static char          lastCalibrationResult[16] = "";
static unsigned long calibResultSetAt    = 0;

static unsigned long s_failVibEndMs      = 0;
static unsigned long s_successPulseEndMs = 0;
// Temporary buffer for successful calibration before profile naming
static float s_lastCalibratedX = 0.0f;
static float s_lastCalibratedY = 0.0f;
static float s_lastCalibratedZ = 0.0f;
static bool  s_lastCalibrationValid = false;

struct CalibrationStats {
    float meanX;
    float meanY;
    float meanZ;
    float stdDevX;
    float stdDevY;
    float stdDevZ;
};

static CalibrationStats calculateCalibrationStats(int sampleLimit);

static uint16_t computeCalibrationQuality(uint32_t sampleCount, const CalibrationStats& stats) {
    float spread = (stats.stdDevX + stats.stdDevY + stats.stdDevZ) / 3.0f;
    float quality = 100.0f - (spread * 25.0f);
    if (sampleCount < MIN_VALID_SAMPLES) {
        quality -= 15.0f;
    }
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 100.0f) quality = 100.0f;
    return (uint16_t)(quality + 0.5f);
}

static const char* calibrationQualityLabel(uint16_t quality) {
    if (quality >= 85) return "Excellent";
    if (quality >= 70) return "Good";
    if (quality >= 50) return "Acceptable";
    return "Fail";
}

static void calibrationStartBlocked(const char* reason) {
    logPacket("CALIB", reason ? reason : "START_BLOCKED");
    notifyCalibrationComplete(false, 0u, "", 0u, 0u, 0u, reason ? reason : "START_BLOCKED");
}

// ── Helpers ─────────────────────────────────────────────────────────────────
static void goToTrainingMode() {
    deviceOn = true;
    setDeviceMode(MODE_TRAINING);
}

static void calibrationFail(const char* reason) {
    calibState = CALIB_STATE_IDLE;
    strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
    lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
    calibResultSetAt = millis();
    
    // Failure pulse: 500ms duration, 150 duty cycle
    motorSetDuty(0);
    s_failVibEndMs = millis() + 500UL;
    motorOverrideDuty(150, 500);

    s_lastCalibrationValid = false;

    // Exact console log format: structured RTT packet
    if (strcmp(reason, "Bad movement") == 0 || strcmp(reason, "Too much movement") == 0) {
        logEvent("CALIB", "bad_movement_failed");
    } else {
        char payload[80];
        snprintf(payload, sizeof(payload), "{\"event\":\"failed\",\"reason\":\"%s\"}", reason);
        logPacket("CALIB", payload);
    }
    notifyCalibrationStatus("failed", "done");

    goToTrainingMode();
}

static void calibrationSuccess(float avgX, float avgY, float avgZ, uint16_t passedSamples) {
    calibState = CALIB_STATE_IDLE;
    strncpy(lastCalibrationResult, "complete", sizeof(lastCalibrationResult) - 1);
    lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
    calibResultSetAt = millis();
    
    s_lastCalibratedX = avgX;
    s_lastCalibratedY = avgY;
    s_lastCalibratedZ = avgZ;
    s_lastCalibrationValid = true;

    logEvent("CALIB", "done");

    CalibrationStats finalStats = calculateCalibrationStats(totalSamples);
    const uint16_t quality = computeCalibrationQuality((uint32_t)totalSamples, finalStats);
    const char* qualityLabel = calibrationQualityLabel(quality);

    const uint32_t profileIdBeforeSave = (getActiveProfile() ? getActiveProfile()->id : 0u);
    const int activeIndexBeforeSave = getActiveProfileIndex();
    const uint8_t slotBeforeSave = (activeIndexBeforeSave >= 0) ? (uint8_t)(activeIndexBeforeSave + 1) : 0u;

    if (quality < 50) {
        logEvent("CALIB", "quality_too_low");
        notifyCalibrationStatus("failed", "done");
        notifyCalibrationComplete(false, 0u, "", 0u, quality, (uint16_t)totalSamples, "LOW_QUALITY");
    } else if (!addNextCalibrationProfile()) {
        logEvent("CALIB", "profile_save_failed");
        notifyCalibrationStatus("failed", "done");
        notifyCalibrationComplete(false, 0u, "", 0u, 0u, (uint16_t)totalSamples, "MOVEMENT_TOO_HIGH");
    } else {
        const OrientationProfile* active = getActiveProfile();
        const uint32_t profileId = active ? active->id : 0u;
        char payload[96];
        snprintf(payload, sizeof(payload), "{\"quality\":\"%s\",\"value\":%u}", qualityLabel, (unsigned)quality);
        logPacket("CALIB", payload);
        notifyCalibrationStatus("success", "done");
        notifyCalibrationComplete(true,
                                  profileId ? profileId : profileIdBeforeSave,
                                  active ? active->name : "",
                                  slotBeforeSave,
                                  quality,
                                  (uint16_t)totalSamples,
                                  nullptr,
                                  avgX, avgY, avgZ,
                                  passedSamples);
    }

    // Start this after profile storage so flash writes cannot stretch the pulse.
    motorSetDuty(0);
    s_successPulseEndMs = millis() + 125UL;
    motorOverrideDuty(150, 125);

    goToTrainingMode();
}

static CalibrationStats calculateCalibrationStats(int sampleLimit) {
    CalibrationStats stats = {0, 0, 0, 0, 0, 0};
    if (sampleLimit <= 0) {
        return stats;
    }

    float sumAllX = 0, sumAllY = 0, sumAllZ = 0;
    for (int i = 0; i < sampleLimit; i++) {
        sumAllX += samplesX[i];
        sumAllY += samplesY[i];
        sumAllZ += samplesZ[i];
    }

    stats.meanX = sumAllX / (float)sampleLimit;
    stats.meanY = sumAllY / (float)sampleLimit;
    stats.meanZ = sumAllZ / (float)sampleLimit;

    float varX = 0, varY = 0, varZ = 0;
    for (int i = 0; i < sampleLimit; i++) {
        float dx = samplesX[i] - stats.meanX;
        float dy = samplesY[i] - stats.meanY;
        float dz = samplesZ[i] - stats.meanZ;
        varX += dx * dx;
        varY += dy * dy;
        varZ += dz * dz;
    }

    stats.stdDevX = sqrtf(varX / (float)sampleLimit);
    stats.stdDevY = sqrtf(varY / (float)sampleLimit);
    stats.stdDevZ = sqrtf(varZ / (float)sampleLimit);
    return stats;
}

static bool calibrationStatsTooUnstable(const CalibrationStats& stats, float limit) {
    return stats.stdDevX > limit || stats.stdDevY > limit || stats.stdDevZ > limit;
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

#if ALIGN_RTT_CALIB_VERBOSE
    static unsigned long lastDebugPrintMs = 0;
    if (currentMillis - lastDebugPrintMs >= 1000UL) {
        lastDebugPrintMs = currentMillis;
        rtt.printf("DEBUG: calibState=%d, elapsed=%lu, totalSamples=%d, dtSample=%lu\n",
                   (int)calibState, elapsed, totalSamples, currentMillis - s_lastSampleTime);
    }
#endif

    if (elapsed > kSafetyTimeoutMs) {
        calibrationFail("Timeout");
        return;
    }

    if (calibState == CALIB_STATE_GET_READY) {
        if (currentMillis - lastHoldPrintMs >= 1000UL) {
            lastHoldPrintMs = currentMillis;
            uint32_t msLeft = (CALIB_GET_READY_MS > elapsed) ? (CALIB_GET_READY_MS - elapsed) : 0;
            int secondsLeft = (msLeft + 500UL) / 1000UL;
            if (secondsLeft > 0) {
                rtt.printf("CALIBRATION: GET READY - %d sec\n", secondsLeft);
            }
        }

        if (elapsed >= CALIB_GET_READY_MS) {
            calibState = CALIB_STATE_HOLD_STILL;
            lastHoldPrintMs = currentMillis;
            s_lastSampleTime = currentMillis - kSampleIntervalMs;
            totalSamples = 0;
            rtt.println("CALIBRATION: HOLD STILL - 5 sec");
        }
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

            if (totalSamples < kMaxCalibrationSamples) {
                samplesX[totalSamples] = rawX;
                samplesY[totalSamples] = rawY;
                samplesZ[totalSamples] = rawZ;
                totalSamples++;

#if ALIGN_RTT_CALIB_VERBOSE
                rtt.printf("CALIB: Sample #%d - raw[%s, %s, %s]\n",
                           totalSamples, String(rawX, 2).c_str(), String(rawY, 2).c_str(), String(rawZ, 2).c_str());
#endif
            }

            if (totalSamples >= kEarlyFailMinSamples && (totalSamples % 10) == 0) {
                CalibrationStats earlyStats = calculateCalibrationStats(totalSamples);
                if (calibrationStatsTooUnstable(earlyStats, kEarlyFailStdDevLimit)) {
                    calibrationFail("Too much movement");
                    return;
                }
            }
        }

        if (elapsed >= CALIB_TOTAL_MS) {
            if (totalSamples == 0) {
                calibrationFail("Too few samples");
                return;
            }

            CalibrationStats finalStats = calculateCalibrationStats(totalSamples);

#if ALIGN_RTT_CALIB_VERBOSE
            rtt.printf("CALIB STATS: Mean[%s, %s, %s], StdDev[%s, %s, %s]\n",
                       String(finalStats.meanX, 2).c_str(), String(finalStats.meanY, 2).c_str(), String(finalStats.meanZ, 2).c_str(),
                       String(finalStats.stdDevX, 2).c_str(), String(finalStats.stdDevY, 2).c_str(), String(finalStats.stdDevZ, 2).c_str());
#endif

            // Safety limit: if standard deviation is too high, posture is too unstable
            if (calibrationStatsTooUnstable(finalStats, kFinalStdDevLimit)) {
                calibrationFail("Too much movement");
                return;
            }

            // Pass 3: Reject outliers (mean +/- 2*sigma) and compute final average
            float finalSumX = 0, finalSumY = 0, finalSumZ = 0;
            int validCount = 0;

            for (int i = 0; i < totalSamples; i++) {
                // If stdDev is near zero, accept all (protect from floating point edge case)
                bool okX = (finalStats.stdDevX < 0.01f) || (fabsf(samplesX[i] - finalStats.meanX) <= 2.0f * finalStats.stdDevX);
                bool okY = (finalStats.stdDevY < 0.01f) || (fabsf(samplesY[i] - finalStats.meanY) <= 2.0f * finalStats.stdDevY);
                bool okZ = (finalStats.stdDevZ < 0.01f) || (fabsf(samplesZ[i] - finalStats.meanZ) <= 2.0f * finalStats.stdDevZ);

                if (okX && okY && okZ) {
                    finalSumX += samplesX[i];
                    finalSumY += samplesY[i];
                    finalSumZ += samplesZ[i];
                    validCount++;
                } else {
#if ALIGN_RTT_CALIB_VERBOSE
                    rtt.printf("CALIB: Outlier Sample #%d rejected - raw[%s, %s, %s]\n",
                               i + 1, String(samplesX[i], 2).c_str(), String(samplesY[i], 2).c_str(), String(samplesZ[i], 2).c_str());
#endif
                }
            }

#if ALIGN_RTT_CALIB_VERBOSE
            rtt.printf("CALIB RESULTS: Valid samples=%d/%d\n", validCount, totalSamples);
#endif

            if (validCount < MIN_VALID_SAMPLES) {
                calibrationFail("Too much movement");
                return;
            }

            const float avgX = finalSumX / (float)validCount;
            const float avgY = finalSumY / (float)validCount;
            const float avgZ = finalSumZ / (float)validCount;
            calibrationSuccess(avgX, avgY, avgZ, (uint16_t)validCount);
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

    if (!sensorInitialized) {
        calibrationStartBlocked("SENSOR_NOT_INITIALIZED");
        return;
    }
    if (therapyIsRunning() || bluetoothIsMotorActive()) {
        calibrationStartBlocked("MOTOR_ACTIVE");
        return;
    }
    if (isDeviceMoving()) {
        calibrationStartBlocked("DEVICE_MOVING");
        return;
    }

    lastCalibrationResult[0] = '\0';
    calibResultSetAt = 0;

    deviceOn = true;
    wakePostureSensor();
    if (therapyIsRunning()) {
        therapyStop(false);
    }

    // Start calibration with start haptic pulse (150 duty for 150ms)
    motorSetDuty(0);
    motorOverrideDuty(150, 150);

    calibState         = CALIB_STATE_GET_READY;
    stabilityStartTime = millis();
    lastHoldPrintMs    = millis();

    totalSamples = 0;

    s_lastSampleTime = millis();

    logEvent("CALIB", "start");
    notifyCalibrationStatus("", "");
    logPacket("CALIB", "{\"phase\":\"GET_READY\",\"seconds\":3}");
}

void cancelCalibration() {
    if (calibState == CALIB_STATE_IDLE) {
        return;
    }
    logEvent("CALIB", "cancelled");
    notifyCalibrationStatus("cancelled", "done");
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
    if ((now - calibResultSetAt) > CALIB_RESULT_BROADCAST_MS) {
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
