#include "motor.h"
#include "config.h"
#include "button.h"

extern int therapyIntensityLevel;

static volatile uint8_t g_dutyApplied = 0;
static volatile uint8_t g_dutyWanted  = 0;
static volatile uint8_t g_overrideDuty = 0;
static volatile uint32_t g_overrideUntilMs = 0;
static volatile uint32_t g_calmStartMs = 0;
static volatile uint16_t g_calmDurationMs = 0;

static constexpr uint8_t CALM_HAPTIC_PEAK_DUTY = 70;

// ── Public API ─────────────────────────────────────────────────────────────
static void applyDuty(uint8_t duty) {
    if (currentMode == MODE_THERAPY) {
        float scale = 0.85f; // Mid
        if (therapyIntensityLevel == 1) scale = 0.70f;
        else if (therapyIntensityLevel == 3) scale = 1.00f;
        duty = (uint8_t)((float)duty * scale);
    }

    if (duty == g_dutyApplied) return;
    g_dutyApplied = duty;

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

static uint8_t calmHapticDuty(uint32_t nowMs) {
    uint32_t elapsed = nowMs - g_calmStartMs;
    uint32_t half = g_calmDurationMs / 2u;
    if (half == 0u) return 0;

    uint32_t level = (elapsed < half)
        ? (elapsed * CALM_HAPTIC_PEAK_DUTY) / half
        : ((g_calmDurationMs - elapsed) * CALM_HAPTIC_PEAK_DUTY) / half;

    return (uint8_t)constrain((int)level, 0, CALM_HAPTIC_PEAK_DUTY);
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
    const uint32_t now = millis();

    // If an override is already active, turn off the motor first to reset the vibration feel.
    if (overrideActive(now)) {
        applyDuty(0);
        delay(30); // 30ms gap to let the motor spin down
    }

    g_overrideDuty = duty;
    g_overrideUntilMs = millis() + durationMs;
    g_calmDurationMs = 0;
    applyDuty(g_overrideDuty);
}

void motorStartCalmHaptic(uint16_t durationMs) {
    if (durationMs == 0u) return;
    g_overrideUntilMs = 0;
    g_calmStartMs = millis();
    g_calmDurationMs = durationMs;
    applyDuty(0);
}

void motorCancelCalmHaptic() {
    g_calmDurationMs = 0;
}

void motorUpdate() {
    const uint32_t now = millis();
    if (overrideActive(now)) {
        applyDuty(g_overrideDuty);
        return;
    }
    if (calmHapticActive(now)) {
        applyDuty(calmHapticDuty(now));
        return;
    }
    applyDuty(g_dutyWanted);
}
