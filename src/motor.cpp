#include "motor.h"
#include "config.h"
#include "button.h"

extern int therapyIntensityLevel;

static volatile uint8_t g_dutyApplied = 0;
static volatile uint8_t g_dutyWanted  = 0;
static volatile uint8_t g_overrideDuty = 0;
static volatile uint32_t g_overrideUntilMs = 0;

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

void motorSetup() {
    pinMode(PIN_MOTOR, OUTPUT);
    digitalWrite(PIN_MOTOR, LOW);
    g_dutyApplied = 0;
    g_dutyWanted = 0;
    g_overrideDuty = 0;
    g_overrideUntilMs = 0;
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
    applyDuty(g_overrideDuty);
}

void motorUpdate() {
    const uint32_t now = millis();
    if (overrideActive(now)) {
        applyDuty(g_overrideDuty);
        return;
    }
    applyDuty(g_dutyWanted);
}
