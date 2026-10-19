#include "motor.h"
#include "config.h"
#include "button.h"
#include "calibration.h"
#include "therapy.h"
#include "training.h"

extern int therapyIntensityLevel;

static volatile uint8_t g_dutyApplied = 0;
static volatile uint8_t g_dutyWanted  = 0;
static volatile uint8_t g_overrideDuty = 0;
static volatile uint32_t g_overrideUntilMs = 0;
static volatile uint32_t g_calmStartMs = 0;
static volatile uint16_t g_calmDurationMs = 0;
static volatile bool g_motorActive = false;
bool motorActive = false;

// Gentle PWM peak for short status haptics. It stays above the ERM motor's
// stiction range without using the harsher high-duty levels.
static constexpr uint8_t CALM_HAPTIC_PEAK_DUTY = 140;

static void refreshMotorActiveState(uint32_t nowMs) {
    motorActive = g_motorActive ||
                  (g_overrideUntilMs != 0u && (int32_t)(nowMs - g_overrideUntilMs) < 0) ||
                  (g_calmDurationMs != 0u && (uint32_t)(nowMs - g_calmStartMs) < g_calmDurationMs) ||
                  isTherapyRunning ||
                  isCalibrationRunning ||
                  isTrainingMotorAlertActive;
}

// ── Public API ─────────────────────────────────────────────────────────────
static void applyDuty(uint8_t duty) {
    if (currentMode == MODE_THERAPY) {
        float scale = 0.75f; // Mid (Level 2)
        if (therapyIntensityLevel == 1) scale = 0.50f;
        else if (therapyIntensityLevel == 3) scale = 1.00f;
        duty = (uint8_t)((float)duty * scale);
    }

    if (duty == g_dutyApplied) return;
    g_dutyApplied = duty;
    g_motorActive = duty != 0;
    refreshMotorActiveState(millis());
    // Use hardware PWM via analogWrite to prevent excessive startup current and brownout resets
    analogWrite(PIN_MOTOR, duty);
}

static bool overrideActive(uint32_t nowMs) {
    if (g_overrideUntilMs == 0u) return false;
    if ((int32_t)(nowMs - g_overrideUntilMs) < 0) return true;
    g_overrideUntilMs = 0u;
    return false;
}

static bool calmHapticActive(uint32_t nowMs) {
    if (g_calmDurationMs == 0u) return false;
    if ((uint32_t)(nowMs - g_calmStartMs) < g_calmDurationMs) return true;
    g_calmDurationMs = 0u;
    return false;
}

// Smoothstep ramp with a short peak dwell. The zero-slope ends avoid the abrupt
// changes of a linear ramp while the dwell still gives the ERM rotor time to spin.
static uint8_t smoothHapticRampDuty(uint32_t positionMs, uint32_t rampMs) {
    if (rampMs == 0u || positionMs >= rampMs) return CALM_HAPTIC_PEAK_DUTY;

    // Fixed-point smoothstep: x*x*(3 - 2*x), with x scaled from 0 to 256.
    const uint32_t x = (positionMs * 256u) / rampMs;
    const uint32_t eased = (x * x * (768u - (2u * x))) >> 16;
    return (uint8_t)((eased * CALM_HAPTIC_PEAK_DUTY) >> 8);
}

static uint8_t calmHapticDuty(uint32_t nowMs) {
    uint32_t elapsed = nowMs - g_calmStartMs;
    uint32_t rampMs = g_calmDurationMs / 3u;
    if (rampMs == 0u) return CALM_HAPTIC_PEAK_DUTY;

    if (elapsed < rampMs) {
        return smoothHapticRampDuty(elapsed, rampMs);
    }

    uint32_t holdEndMs = g_calmDurationMs - rampMs;
    if (elapsed < holdEndMs) {
        return CALM_HAPTIC_PEAK_DUTY;
    }

    uint32_t remaining = (g_calmDurationMs > elapsed) ? (g_calmDurationMs - elapsed) : 0u;
    return smoothHapticRampDuty(remaining, rampMs);
}

void motorSetup() {
    pinMode(PIN_MOTOR, OUTPUT);
    digitalWrite(PIN_MOTOR, LOW);
    g_dutyApplied = 0;
    g_dutyWanted = 0;
    g_overrideDuty = 0;
    g_overrideUntilMs = 0;
    g_calmStartMs = 0;
    g_calmDurationMs = 0;
    g_motorActive = false;
    motorActive = false;
}

void motorSetDuty(uint8_t duty) {
    g_dutyWanted = duty;
    const uint32_t now = millis();
    if (overrideActive(now)) {
        return; // Keep temporary feedback pulse authoritative.
    }
    applyDuty(duty);
}

void motorOverrideDuty(uint8_t duty, uint16_t durationMs) {
    if (durationMs == 0u) return;

    g_overrideDuty = duty;
    g_overrideUntilMs = millis() + durationMs;
    g_calmDurationMs = 0;
    applyDuty(g_overrideDuty);
    refreshMotorActiveState(millis());
}

void motorStartCalmHaptic(uint16_t durationMs) {
    if (durationMs == 0u) return;
    g_overrideUntilMs = 0;
    g_calmStartMs = millis();
    g_calmDurationMs = durationMs;
    applyDuty(0);
    refreshMotorActiveState(millis());
}

void motorCancelCalmHaptic() {
    bool wasActive = g_calmDurationMs != 0;
    g_calmDurationMs = 0;
    if (wasActive) {
        applyDuty(g_dutyWanted);
    }
    refreshMotorActiveState(millis());
}

void motorCancelFeedback() {
    g_overrideUntilMs = 0;
    g_overrideDuty = 0;
    g_calmDurationMs = 0;
    applyDuty(g_dutyWanted);
    refreshMotorActiveState(millis());
}

bool motorIsActive() {
    return motorActive;
}

bool motorOutputIsOn() {
    return g_dutyApplied != 0u;
}

void motorUpdate() {
    const uint32_t now = millis();
    if (overrideActive(now)) {
        applyDuty(g_overrideDuty);
        refreshMotorActiveState(now);
        return;
    }
    if (calmHapticActive(now)) {
        applyDuty(calmHapticDuty(now));
        refreshMotorActiveState(now);
        return;
    }
    applyDuty(g_dutyWanted);
    refreshMotorActiveState(now);
}
