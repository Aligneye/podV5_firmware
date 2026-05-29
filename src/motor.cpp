#include "motor.h"
#include "config.h"
#include "nrf.h"
#include "button.h"

extern int therapyIntensityLevel;

// ── Hardware timer PWM via TIMER1 ──────────────────────────────────────────
// TIMER1 is free on nRF52832 when no SoftDevice is present (TIMER0 is used
// by SD; TIMER1/2/3/4 are application-available). We configure TIMER1 in
// 32-bit mode at 1 MHz tick, using two CC channels:
//   CC[0] = ON time  (fires → pin HIGH)
//   CC[1] = period   (fires → pin LOW, reloads CC[0] for next cycle)
//
// PWM period = 10 ms (100 Hz) — slow enough that loop jitter is irrelevant,
// fast enough that a vibration motor feels it as continuous intensity.
//
// duty 0   → motor always off (timer not started)
// duty 255 → motor always on  (timer not started, pin held HIGH)
// 1–254    → timer runs, CC[0] = (duty * 10000) / 255 µs

static constexpr uint32_t TIMER_FREQ_HZ  = 1000000UL;  // 1 MHz prescaler
static constexpr uint32_t PWM_PERIOD_US  = 10000UL;     // 10 ms = 100 Hz

static volatile uint8_t g_dutyApplied = 0;
static volatile uint8_t g_dutyWanted  = 0;
static volatile uint8_t g_overrideDuty = 0;
static volatile uint32_t g_overrideUntilMs = 0;

// ── TIMER1 IRQ ─────────────────────────────────────────────────────────────
extern "C" void TIMER1_IRQHandler(void) {
    if (NRF_TIMER1->EVENTS_COMPARE[0]) {
        NRF_TIMER1->EVENTS_COMPARE[0] = 0;
        // ON time elapsed → pull pin LOW (end of pulse)
        NRF_GPIO->OUTCLR = (1UL << g_ADigitalPinMap[PIN_MOTOR]);
    }
    if (NRF_TIMER1->EVENTS_COMPARE[1]) {
        NRF_TIMER1->EVENTS_COMPARE[1] = 0;
        // Period elapsed → pull pin HIGH (start of next pulse), reload timer
        NRF_TIMER1->TASKS_CLEAR = 1;
        NRF_GPIO->OUTSET = (1UL << g_ADigitalPinMap[PIN_MOTOR]);
    }
}

// ── Internal helpers ───────────────────────────────────────────────────────
static void timerStop() {
    NRF_TIMER1->TASKS_STOP  = 1;
    NRF_TIMER1->TASKS_CLEAR = 1;
    NVIC_DisableIRQ(TIMER1_IRQn);
    NRF_GPIO->OUTCLR = (1UL << g_ADigitalPinMap[PIN_MOTOR]);
}

static void timerStart(uint8_t duty) {
    uint32_t onUs = ((uint32_t)duty * PWM_PERIOD_US) / 255u;

    NRF_TIMER1->TASKS_STOP  = 1;
    NRF_TIMER1->TASKS_CLEAR = 1;

    // 1 MHz → prescaler = 4  (16 MHz / 2^4 = 1 MHz)
    NRF_TIMER1->PRESCALER  = 4;
    NRF_TIMER1->BITMODE    = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER1->MODE       = TIMER_MODE_MODE_Timer;
    NRF_TIMER1->SHORTS     = 0;  // no auto-clear; we clear in CC[1] handler

    NRF_TIMER1->CC[0] = onUs;           // rising → falling edge
    NRF_TIMER1->CC[1] = PWM_PERIOD_US;  // falling → rising edge (period end)

    NRF_TIMER1->EVENTS_COMPARE[0] = 0;
    NRF_TIMER1->EVENTS_COMPARE[1] = 0;

    NRF_TIMER1->INTENSET =
        (TIMER_INTENSET_COMPARE0_Enabled << TIMER_INTENSET_COMPARE0_Pos) |
        (TIMER_INTENSET_COMPARE1_Enabled << TIMER_INTENSET_COMPARE1_Pos);

    NVIC_SetPriority(TIMER1_IRQn, 3);
    NVIC_ClearPendingIRQ(TIMER1_IRQn);
    NVIC_EnableIRQ(TIMER1_IRQn);

    // Pull pin HIGH to start first pulse, then let timer pull it LOW at CC[0]
    NRF_GPIO->OUTSET = (1UL << g_ADigitalPinMap[PIN_MOTOR]);
    NRF_TIMER1->TASKS_START = 1;
}

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
    g_overrideDuty = duty;
    g_overrideUntilMs = now + durationMs;
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
