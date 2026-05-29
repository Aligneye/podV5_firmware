#include "step_count.h"
#include "device_time.h"
#include <math.h>

namespace {
static constexpr float kGravityRef         = 9.81f;
static constexpr float kHighPassAlpha      = 0.90f;
static constexpr float kStepThreshold      = 1.15f;
static constexpr float kStepHysteresis     = 0.55f;
static constexpr uint32_t kMinStepGapMs    = 280UL;
static constexpr uint32_t kMaxStepGapMs    = 2000UL;

static float s_hp = 0.0f;
static float s_prevMag = kGravityRef;
static bool s_armed = true;
static uint32_t s_lastStepMs = 0;
static uint32_t s_totalSteps = 0;
static uint32_t s_lastResetEpochDay = 0;
static uint32_t s_lastResetUptimeDay = 0;

static void maybeResetDaily() {
    if (isDeviceTimeSynced()) {
        const uint32_t epoch = getDeviceTime();
        if (epoch == 0u) return;

        const uint32_t day = epoch / 86400u;
        if (s_lastResetEpochDay == 0u) {
            s_lastResetEpochDay = day;
            return;
        }
        if (day != s_lastResetEpochDay) {
            stepCountReset();
            s_lastResetEpochDay = day;
        }
        return;
    }

    const uint32_t upSec = getDeviceUptimeSeconds();
    const uint32_t upDay = upSec / 86400u;
    if (s_lastResetUptimeDay == 0u) {
        s_lastResetUptimeDay = upDay;
        return;
    }
    if (upDay != s_lastResetUptimeDay) {
        stepCountReset();
        s_lastResetUptimeDay = upDay;
    }
}
}

void stepCountInit() {
    s_hp = 0.0f;
    s_prevMag = kGravityRef;
    s_armed = true;
    s_lastStepMs = 0;
    s_totalSteps = 0;
    s_lastResetEpochDay = 0;
    s_lastResetUptimeDay = 0;
}

void stepCountProcessSample(float ax, float ay, float az, uint32_t nowMs) {
    maybeResetDaily();

    const float mag = sqrtf(ax * ax + ay * ay + az * az);

    // One-pole high-pass on acceleration magnitude.
    s_hp = kHighPassAlpha * (s_hp + mag - s_prevMag);
    s_prevMag = mag;
    const float signal = fabsf(s_hp);

    const uint32_t sinceLast = nowMs - s_lastStepMs;
    if (sinceLast > kMaxStepGapMs) {
        s_armed = true;
    }

    if (s_armed && signal >= kStepThreshold && sinceLast >= kMinStepGapMs) {
        s_totalSteps++;
        s_lastStepMs = nowMs;
        s_armed = false;
        return;
    }

    if (!s_armed && signal <= kStepHysteresis) {
        s_armed = true;
    }
}

uint32_t stepCountGetTotal() {
    return s_totalSteps;
}

void stepCountReset() {
    s_totalSteps = 0;
    s_lastStepMs = 0;
    s_armed = true;
}
