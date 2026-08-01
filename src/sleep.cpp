#include "sleep.h"
#include "bluetooth.h"
#include "button.h"
#include "calibration.h"
#include "config.h"
#include "motor.h"
#include "rtt_debugger.h"
#include "training.h"
#include "nrf_gpio.h"

extern RTTStream rtt;
static AlignRttSilencer s_nonBleRtt;
#define rtt s_nonBleRtt

static uint32_t s_timerStartedMs = 0;
static bool s_timerRunning = false;
static bool s_sleeping = false;
static constexpr uint32_t kIdleDisconnectedSleepTimeoutMs = 5000UL;
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

static bool sleepTimerEligible() {
    return currentMode == MODE_IDLE &&
           !bluetoothIsConnected() &&
           !isCalibrating();
}

static void enterSleepMode() {
    // State may have changed after the timeout check earlier in this loop.
    if (s_sleeping || !sleepTimerEligible()) {
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

    blinkSleepLed(true, false, false);
    blinkSleepLed(false, false, true);
    blinkSleepLed(false, true, false);
    hardReleaseSleepLeds();

    pulseMotorForSleep();
    hardReleaseMotor();

    rtt.println("Entering System OFF");
    // Framework wrapper maps the Arduino pin, configures pull-up/SENSE_LOW,
    // and selects the SoftDevice or direct POWER-register path as needed.
    systemOff(PIN_BUTTON, LOW);

    // On real hardware System OFF never returns. With an attached debugger the
    // nRF52832 emulates System OFF and keeps the CPU powered, so do not execute
    // normal firmware or force a reset after the call.
    while (true) {
        waitForEvent();
    }
}

void sleepTimerSetup() {
    s_timerStartedMs = 0;
    s_timerRunning = false;
    s_sleeping = false;
}

void sleepTimerLoop() {
    const uint32_t now = millis();

    if (!sleepTimerEligible()) {
        s_timerStartedMs = 0;
        s_timerRunning = false;
        return;
    }

    if (!s_timerRunning) {
        s_timerStartedMs = now;
        s_timerRunning = true;
        logEvent("SLEEP", "idle_disconnected_timer_start");
        return;
    }

    if (!s_sleeping && (now - s_timerStartedMs) >= kIdleDisconnectedSleepTimeoutMs) {
        enterSleepMode();
    }
}
