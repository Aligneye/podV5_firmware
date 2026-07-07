#include "sleep.h"
#include "rtt_debugger.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "nrf_soc.h"

extern RTTStream rtt;

static uint32_t s_lastResetMs = 0;
static bool s_prevActive = false;
static bool s_sleeping = false;
static uint32_t s_holdoffUntilMs = 0;
static constexpr uint32_t kInactivitySleepTimeoutMs = 5000UL;
static constexpr uint16_t kSleepLedBlinkOnMs = 80;
static constexpr uint16_t kSleepLedBlinkGapMs = 40;
static constexpr uint16_t kSleepMotorPulseMs = 120;
static constexpr uint16_t kSleepMotorSettleMs = 20;

static void setSleepLedState(bool redOn, bool greenOn, bool blueOn) {
    analogWrite(PIN_LED_RED, redOn ? 0 : 255);
    analogWrite(PIN_LED_GREEN, greenOn ? 0 : 255);
    analogWrite(PIN_LED_BLUE, blueOn ? 0 : 255);
}

static void hardReleaseSleepLeds() {
    setSleepLedState(false, false, false);
    nrf_gpio_cfg_default(PIN_LED_RED);
    nrf_gpio_cfg_default(PIN_LED_GREEN);
    nrf_gpio_cfg_default(PIN_LED_BLUE);
}

static void blinkSleepLed(bool redOn, bool greenOn, bool blueOn) {
    setSleepLedState(redOn, greenOn, blueOn);
    delay(kSleepLedBlinkOnMs);
    setSleepLedState(false, false, false);
    delay(kSleepLedBlinkGapMs);
}

static void pulseMotorForSleep() {
    motorSetDuty(VIB_INTENSITY_MAX);
    delay(kSleepMotorPulseMs);
    motorSetDuty(0);
    delay(kSleepMotorSettleMs);
}

static void hardReleaseMotor() {
    motorSetDuty(0);
    nrf_gpio_cfg_default(PIN_MOTOR);
}

static void armButtonWakeSource() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    nrf_gpio_cfg_sense_input(PIN_BUTTON, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
}

static void enterSleepMode() {
    if (s_sleeping) {
        return;
    }

    s_sleeping = true;
    logEvent("SLEEP", "enter_idle_then_off");

    if (currentMode != MODE_IDLE) {
        setDeviceMode(MODE_IDLE);
    }

    motorCancelFeedback();
    motorSetDuty(0);
    bluetoothPrepareForSleep();
    sleepPostureSensor();
    armButtonWakeSource();

    blinkSleepLed(true, false, false);
    blinkSleepLed(false, false, true);
    blinkSleepLed(false, true, false);
    hardReleaseSleepLeds();

    pulseMotorForSleep();
    hardReleaseMotor();

    rtt.println("Entering System OFF");
    sd_power_system_off();
    rtt.println("Returned from System OFF");
}

bool inactivityTimerHasActivity() {
    return isTherapyRunning ||
           isCalibrationRunning ||
           motorActive ||
           isTrainingMotorAlertActive;
}

void inactivityTimerSetup() {
    s_lastResetMs = millis();
    s_prevActive = false;
}

void inactivityTimerReset() {
    s_lastResetMs = millis();
}

void inactivityTimerHoldoffAfterCalibration(uint32_t delayMs) {
    const uint32_t now = millis();
    const uint32_t until = now + delayMs;
    if ((int32_t)(until - s_holdoffUntilMs) > 0) {
        s_holdoffUntilMs = until;
    }
}

void inactivityTimerLoop() {
    const uint32_t now = millis();
    const bool holdoffActive = (s_holdoffUntilMs != 0u) &&
                               ((int32_t)(now - s_holdoffUntilMs) < 0);
    if (!holdoffActive && s_holdoffUntilMs != 0u) {
        s_holdoffUntilMs = 0u;
    }

    if (holdoffActive) {
        s_lastResetMs = now;
        s_prevActive = true;
        return;
    }

    const bool active = inactivityTimerHasActivity();

    if (active) {
        s_lastResetMs = now;
        if (!s_prevActive) {
            logEvent("INACTIVITY", "timer_reset");
        }
    } else if (!s_sleeping && (now - s_lastResetMs) >= kInactivitySleepTimeoutMs) {
        enterSleepMode();
    }

    s_prevActive = active;
}

uint32_t inactivityTimerGetElapsedMs() {
    return millis() - s_lastResetMs;
}

uint32_t inactivityTimerGetLastResetMs() {
    return s_lastResetMs;
}
