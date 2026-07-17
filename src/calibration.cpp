#include "calibration.h"
#include "bluetooth.h"
#include "button.h"
#include "sleep.h"
#include "motor.h"
#include "therapy.h"
#include "training.h"
#include "storage.h"
#include "rtt_debugger.h"
#include <math.h>
#include <string.h>

extern RTTStream rtt;
static RTTStream& s_rttTiming = rtt;
static AlignRttSilencer s_nonBleRtt;
#define rtt s_nonBleRtt

// ── State Machine Timings ───────────────────────────────────────────────────
enum CalibState { CALIB_STATE_IDLE, CALIB_STATE_GET_READY, CALIB_STATE_HOLD_STILL };
static CalibState calibState = CALIB_STATE_IDLE;

static constexpr uint32_t CALIB_GET_READY_MS       = 3000UL;
static constexpr uint32_t CALIB_HOLD_MS            = 5000UL;
static constexpr uint32_t CALIB_TOTAL_MS           = CALIB_GET_READY_MS + CALIB_HOLD_MS;
static constexpr uint32_t CALIB_RESULT_BROADCAST_MS = 4000UL;
static constexpr uint32_t kSafetyTimeoutMs         = CALIB_TOTAL_MS + 2000UL;
static constexpr uint32_t kSampleIntervalMs        = 50UL;
static constexpr uint32_t kBleProgressIntervalMs   = 1000UL;
static constexpr int      kMaxCalibrationSamples   = 200;
static constexpr int      MIN_VALID_SAMPLES        = 70;
static constexpr int      kEarlyFailMinSamples     = 40;
static constexpr float    kFinalStdDevLimit        = 1.0f;
static constexpr float    kEarlyFailStdDevLimit    = 1.75f;
// Adaptive early finish: once enough samples exist to clear the full
// validation bar, calibration completes as soon as the result would score
// "Excellent" — a steady user finishes in ~4 s of hold instead of 5 s, and a
// wobbly one still gets the whole window. Never lowers the quality bar.
static constexpr int      kEarlyFinishMinSamples   = 75;
static constexpr uint16_t kEarlyFinishQuality      = 85;
static constexpr int      kStatsCheckEverySamples  = 5;

// Calm result haptics: smooth ramp-up/ramp-down "breathing" pulse rather than a hard buzz.
// Fail rings noticeably longer than success so the two are distinguishable by feel alone.
static constexpr uint16_t kCalibSuccessHapticMs    = 350U;
static constexpr uint16_t kCalibFailHapticMs       = 750U;

static volatile bool pendingStart  = false;
static volatile bool pendingCancel = false;
bool isCalibrationRunning = false;

static unsigned long stabilityStartTime = 0;
static unsigned long lastHoldPrintMs     = 0;
static unsigned long s_lastBleProgressMs = 0;

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
static char  s_pendingProfileName[24] = "";
static uint32_t s_pendingPersistTicket = 0;
static uint32_t s_pendingPersistProfileId = 0;

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
    notifyCalibrationComplete(false, 0u, "", 0u, 0u, reason ? reason : "START_BLOCKED");
}

// ── Helpers ─────────────────────────────────────────────────────────────────
static void goToTrainingMode() {
    deviceOn = true;
    setDeviceMode(MODE_TRAINING);
    s_pendingProfileName[0] = '\0';
}

static void goToIdleMode() {
    deviceOn = true;
    setDeviceMode(MODE_IDLE);
    s_pendingProfileName[0] = '\0';
}

static void calibrationFail(const char* reason) {
    strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
    lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
    calibResultSetAt = millis();

    s_lastCalibrationValid = false;

    // Exact console log format: structured RTT packet
    if (strcmp(reason, "MOVEMENT_TOO_HIGH") == 0) {
        logEvent("CALIB", "bad_movement_failed");
    } else {
        char payload[80];
        snprintf(payload, sizeof(payload), "{\"event\":\"failed\",\"reason\":\"%s\"}", reason);
        logPacket("CALIB", payload);
    }
    calibState = CALIB_STATE_IDLE;
    isCalibrationRunning = false;
    notifyCalibrationStatus("failed", "done");
    notifyCalibrationComplete(false, 0u, "", 0u, (uint16_t)totalSamples, reason);
    goToIdleMode();

    // Calm, longer ring on failure — smooth ramp up/down, no hard buzz.
    // Started after the mode transition so setDeviceMode()'s motorCancelFeedback() can't wipe it.
    s_failVibEndMs = millis() + kCalibFailHapticMs;
    motorStartCalmHaptic(kCalibFailHapticMs);

    inactivityTimerHoldoffAfterCalibration();
}

static void calibrationSuccess(float avgX, float avgY, float avgZ, uint16_t passedSamples,
                               const CalibrationStats& stats) {
    const uint32_t successStartedMs = millis();
    const uint16_t quality = computeCalibrationQuality((uint32_t)passedSamples, stats);
    const char* qualityLabel = calibrationQualityLabel(quality);

    s_lastCalibratedX = avgX;
    s_lastCalibratedY = avgY;
    s_lastCalibratedZ = avgZ;
    s_lastCalibrationValid = true;   // needed because addNextCalibrationProfile() may read this

    if (quality < 50) {
        strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
        lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
        calibResultSetAt = millis();

        s_lastCalibrationValid = false;

        logEvent("CALIB", "quality_too_low");
        calibState = CALIB_STATE_IDLE;
        isCalibrationRunning = false;
        notifyCalibrationStatus("failed", "done");
        notifyCalibrationComplete(false, 0u, "", quality, passedSamples, "LOW_QUALITY");
        goToIdleMode();

        s_failVibEndMs = millis() + kCalibFailHapticMs;
        motorStartCalmHaptic(kCalibFailHapticMs);

        inactivityTimerHoldoffAfterCalibration();
        return;
    }

    logEvent("CALIB", "success_enter");

    bool saveSuccess = false;
    logEvent("CALIB", "before_profile_save");
    if (s_pendingProfileName[0] != '\0') {
        saveSuccess = addCalibrationProfile(s_pendingProfileName);
    } else {
        saveSuccess = addNextCalibrationProfile();
    }
    logEvent("CALIB", saveSuccess ? "after_profile_save_ok" : "after_profile_save_fail");

    if (!saveSuccess) {
        strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
        lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
        calibResultSetAt = millis();

        s_lastCalibrationValid = false;

        logEvent("CALIB", "profile_save_failed");
        calibState = CALIB_STATE_IDLE;
        isCalibrationRunning = false;
        notifyCalibrationStatus("failed", "done");
        notifyCalibrationComplete(false, 0u, "", quality, passedSamples, "PROFILE_SAVE_FAILED");
        goToIdleMode();

        s_failVibEndMs = millis() + kCalibFailHapticMs;
        motorStartCalmHaptic(kCalibFailHapticMs);

        inactivityTimerHoldoffAfterCalibration();
        return;
    }

    strncpy(lastCalibrationResult, "complete", sizeof(lastCalibrationResult) - 1);
    lastCalibrationResult[sizeof(lastCalibrationResult) - 1] = '\0';
    calibResultSetAt = millis();

    const OrientationProfile* active = getActiveProfile();
    const uint32_t profileId = active ? active->id : 0u;
    s_pendingPersistTicket = storageGetLatestDeferredPersistTicket();
    s_pendingPersistProfileId = profileId;

    s_rttTiming.print("[TIMING] calibration RAM save: ");
    s_rttTiming.print(millis() - successStartedMs);
    s_rttTiming.println(" ms");

    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"quality\":\"%s\",\"value\":%u}",
             qualityLabel,
             (unsigned)quality);
    logPacket("CALIB", payload);
    logEvent("CALIB", "done");

    // Keep this as "complete" for app compatibility.
    logEvent("CALIB", "before_notify_done");
    calibState = CALIB_STATE_IDLE;
    isCalibrationRunning = false;
    notifyCalibrationStatus("complete", "done");

    notifyCalibrationComplete(true,
                              profileId,
                              active ? active->name : "",
                              quality,
                              passedSamples,
                              nullptr,
                              avgX, avgY, avgZ,
                              passedSamples);
    s_rttTiming.print("[TIMING] calibration result dispatched: ");
    s_rttTiming.print(millis() - successStartedMs);
    s_rttTiming.println(" ms");
    logEvent("CALIB", "after_notify_done");

    goToTrainingMode();

    // Calm, brief ring on success — smooth ramp up/down, no hard buzz.
    // Started after the mode transition so setDeviceMode()'s motorCancelFeedback() can't wipe it.
    s_successPulseEndMs = millis() + kCalibSuccessHapticMs;
    motorStartCalmHaptic(kCalibSuccessHapticMs);

    inactivityTimerHoldoffAfterCalibration();
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

// Runs the final validation pipeline (stability gate → 2σ outlier rejection →
// minimum-valid-sample gate) over the samples collected so far. Returns true
// when calibration is concluded (success or failure) and the caller should
// stop. On an early attempt (finalAttempt == false) any shortfall just means
// "keep sampling", and success additionally requires an Excellent quality
// score so finishing early can never degrade the stored result.
static bool tryFinalizeCalibration(bool finalAttempt, const CalibrationStats& stats) {
    if (calibrationStatsTooUnstable(stats, kFinalStdDevLimit)) {
        if (finalAttempt) {
            calibrationFail("MOVEMENT_TOO_HIGH");
            return true;
        }
        return false;
    }

    // Reject outliers (mean +/- 2*sigma) and compute the final average.
    float finalSumX = 0, finalSumY = 0, finalSumZ = 0;
    int validCount = 0;

    for (int i = 0; i < totalSamples; i++) {
        // If stdDev is near zero, accept all (protect from floating point edge case)
        bool okX = (stats.stdDevX < 0.01f) || (fabsf(samplesX[i] - stats.meanX) <= 2.0f * stats.stdDevX);
        bool okY = (stats.stdDevY < 0.01f) || (fabsf(samplesY[i] - stats.meanY) <= 2.0f * stats.stdDevY);
        bool okZ = (stats.stdDevZ < 0.01f) || (fabsf(samplesZ[i] - stats.meanZ) <= 2.0f * stats.stdDevZ);

        if (okX && okY && okZ) {
            finalSumX += samplesX[i];
            finalSumY += samplesY[i];
            finalSumZ += samplesZ[i];
            validCount++;
        } else if (finalAttempt) {
#if ALIGN_RTT_CALIB_VERBOSE
            rtt.printf("CALIB: Outlier Sample #%d rejected - raw[%s, %s, %s]\n",
                       i + 1, String(samplesX[i], 2).c_str(), String(samplesY[i], 2).c_str(), String(samplesZ[i], 2).c_str());
#endif
        }
    }

#if ALIGN_RTT_CALIB_VERBOSE
    if (finalAttempt) {
        rtt.printf("CALIB RESULTS: Valid samples=%d/%d\n", validCount, totalSamples);
    }
#endif

    if (validCount < MIN_VALID_SAMPLES) {
        if (finalAttempt) {
            calibrationFail("MOVEMENT_TOO_HIGH");
            return true;
        }
        return false;
    }

    if (!finalAttempt) {
        const uint16_t quality = computeCalibrationQuality((uint32_t)validCount, stats);
        if (quality < kEarlyFinishQuality) {
            return false;
        }
        logEvent("CALIB", "early_finish");
    }

    const float avgX = finalSumX / (float)validCount;
    const float avgY = finalSumY / (float)validCount;
    const float avgZ = finalSumZ / (float)validCount;

    logEvent("CALIB", "before_success");
    calibrationSuccess(avgX, avgY, avgZ, (uint16_t)validCount, stats);
    logEvent("CALIB", "after_success");
    return true;
}

// ── Temporary Calibration Results retrieval ─────────────────────────────────
float getLastCalibratedX() { return s_lastCalibratedX; }
float getLastCalibratedY() { return s_lastCalibratedY; }
float getLastCalibratedZ() { return s_lastCalibratedZ; }
bool isLastCalibrationValid() { return s_lastCalibrationValid; }

void setPendingCalibrationName(const char* name) {
    if (name) {
        strncpy(s_pendingProfileName, name, sizeof(s_pendingProfileName) - 1);
        s_pendingProfileName[sizeof(s_pendingProfileName) - 1] = '\0';
    } else {
        s_pendingProfileName[0] = '\0';
    }
}

// ── Lifecycle ───────────────────────────────────────────────────────────────
void initCalibration() {
    calibState   = CALIB_STATE_IDLE;
    isCalibrationRunning = false;
    pendingStart = false;
    pendingCancel = false;
    motorSetDuty(0);
    s_failVibEndMs      = 0;
    s_successPulseEndMs = 0;
    s_lastSampleTime    = 0;
    s_lastBleProgressMs = 0;
    s_lastCalibrationValid = false;
    totalSamples = 0;

    initProfiles();
}

void handleCalibration() {
    const unsigned long currentMillis = millis();

    if (s_pendingPersistTicket != 0u) {
        bool success = false;
        uint32_t totalMs = 0;
        uint32_t flashMs = 0;
        if (storageGetDeferredPersistResult(s_pendingPersistTicket, &success, &totalMs, &flashMs)) {
            notifyCalibrationPersisted(s_pendingPersistProfileId, success, totalMs, flashMs);
            s_rttTiming.print("[TIMING] calibration durable save: ");
            s_rttTiming.print(totalMs);
            s_rttTiming.print(" ms total, ");
            s_rttTiming.print(flashMs);
            s_rttTiming.println(" ms flash");
            s_pendingPersistTicket = 0u;
            s_pendingPersistProfileId = 0u;
        }
    }

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
    if ((currentMillis - s_lastBleProgressMs) >= kBleProgressIntervalMs) {
        s_lastBleProgressMs = currentMillis;
        notifyCalibrationStatus("", "");
    }

#if ALIGN_RTT_CALIB_VERBOSE
    static unsigned long lastDebugPrintMs = 0;
    if (currentMillis - lastDebugPrintMs >= 1000UL) {
        lastDebugPrintMs = currentMillis;
        rtt.printf("DEBUG: calibState=%d, elapsed=%lu, totalSamples=%d, dtSample=%lu\n",
                   (int)calibState, elapsed, totalSamples, currentMillis - s_lastSampleTime);
    }
#endif

    if (elapsed > kSafetyTimeoutMs) {
        calibrationFail("TIMEOUT");
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
            s_lastBleProgressMs = currentMillis;
            s_lastSampleTime = currentMillis - kSampleIntervalMs;
            totalSamples = 0;
            notifyCalibrationStatus("", "");
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
                calibrationFail("SENSOR_LOST");
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

            if (totalSamples >= kEarlyFailMinSamples &&
                (totalSamples % kStatsCheckEverySamples) == 0) {
                CalibrationStats runningStats = calculateCalibrationStats(totalSamples);
                if (calibrationStatsTooUnstable(runningStats, kEarlyFailStdDevLimit)) {
                    calibrationFail("MOVEMENT_TOO_HIGH");
                    return;
                }
                // Adaptive early finish: complete now if the result would
                // already be Excellent (see tryFinalizeCalibration).
                if (totalSamples >= kEarlyFinishMinSamples &&
                    tryFinalizeCalibration(false, runningStats)) {
                    return;
                }
            }
        }

        if (elapsed >= CALIB_TOTAL_MS) {
            logEvent("CALIB", "end_reached");

            if (totalSamples == 0) {
                calibrationFail("TOO_FEW_SAMPLES");
                return;
            }

            logEvent("CALIB", "calculating_stats");
            CalibrationStats finalStats = calculateCalibrationStats(totalSamples);
            logEvent("CALIB", "stats_done");

#if ALIGN_RTT_CALIB_VERBOSE
            rtt.printf("CALIB STATS: Mean[%s, %s, %s], StdDev[%s, %s, %s]\n",
                       String(finalStats.meanX, 2).c_str(), String(finalStats.meanY, 2).c_str(), String(finalStats.meanZ, 2).c_str(),
                       String(finalStats.stdDevX, 2).c_str(), String(finalStats.stdDevY, 2).c_str(), String(finalStats.stdDevZ, 2).c_str());
#endif

            tryFinalizeCalibration(true, finalStats);
            return;
        }
    }
}

void requestCalibrationStart() {
    pendingStart = true;
}

void requestCalibrationCancel() {
    pendingCancel = true;
}

void startCalibration() {
    if (calibState != CALIB_STATE_IDLE) {
        return;
    }

    // Put the device into a quiet neutral state before sampling.
    therapyStop(false);
    setDeviceMode(MODE_IDLE);
    motorSetDuty(0);

    if (!sensorInitialized) {
        calibrationStartBlocked("SENSOR_NOT_INITIALIZED");
        return;
    }

    lastCalibrationResult[0] = '\0';
    calibResultSetAt = 0;

    deviceOn = true;
    wakePostureSensor();

    // Start calibration with start haptic pulse (150 duty for 150ms)
    motorOverrideDuty(150, 150);

    calibState         = CALIB_STATE_GET_READY;
    isCalibrationRunning  = true;
    stabilityStartTime = millis();
    lastHoldPrintMs    = millis();
    s_lastBleProgressMs = stabilityStartTime;
    inactivityTimerReset();

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
    calibState = CALIB_STATE_IDLE;
    isCalibrationRunning = false;
    notifyCalibrationStatus("cancelled", "done");
    motorSetDuty(0);
    motorUpdate();
    s_failVibEndMs      = 0;
    s_successPulseEndMs = 0;
    s_lastCalibrationValid = false;

    deviceOn = true;
    s_pendingProfileName[0] = '\0';

    // No forced setDeviceMode(MODE_IDLE) here: calibration is only ever cancelled either
    // while currentMode is already Idle (nothing to do), or after a button handler
    // (single-click/hold in button.cpp) has already redirected to Training/Therapy earlier
    // in the same loop() iteration as a side effect of calling setDeviceMode() itself.
    // Forcing Idle here would stomp that redirect back to Idle.
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
    return isCalibrationRunning;
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
