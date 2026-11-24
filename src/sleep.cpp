#include "sleep.h"
#include "bluetooth.h"
#include "button.h"
#include "calibration.h"
#include "config.h"
#include "motor.h"
#include "rtt_debugger.h"
#include "training.h"
#include "session_stats.h"
#include "session_log.h"
#include "nrf_gpio.h"
#include "nrf.h"
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

// Marker file meaning "the user powered the pod off". RESETREAS cannot be used
// to detect a wake from System OFF: the bootloader clears it before the app
// runs (observed RESETREAS=0 on every boot), so the hold-to-power-on check
// relies on this flash marker instead.
static const char* kPowerOffMarkerPath = "/pwroff.flg";
// Measured from app start; the bootloader already consumed part of the hold.
static constexpr uint32_t kPowerOnHoldMs = 800UL;

static void writePowerOffMarker() {
    InternalFS.begin();
    File f = InternalFS.open(kPowerOffMarkerPath, FILE_O_WRITE);
    if (f) {
        const uint8_t v = 1;
        f.write(&v, 1);
        f.close();
    }
}

extern RTTStream rtt;
static AlignRttSilencer s_nonBleRtt;
#define rtt s_nonBleRtt

static uint32_t s_timerStartedMs = 0;
static bool s_timerRunning = false;
static bool s_sleeping = false;
static constexpr uint32_t kIdleDisconnectedSleepTimeoutMs = 120000UL;
static constexpr uint16_t kSleepLedBlinkOnMs = 80;
static constexpr uint16_t kSleepLedBlinkGapMs = 40;

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

static void hardReleaseMotor() {
    motorSetDuty(0);
    // Actively drive the motor driver input LOW. A floating (default/input)
    // pin can let the driver gate drift on and buzz the motor hard while the
    // pod is "off". GPIO output state is retained through System OFF.
    const uint32_t motorPin = g_ADigitalPinMap[PIN_MOTOR];
    nrf_gpio_pin_clear(motorPin);
    nrf_gpio_cfg_output(motorPin);
}

static bool sleepTimerEligible() {
    return currentMode == MODE_IDLE &&
           !bluetoothIsConnected() &&
           !isCalibrating();
}

static void enterSleepModeInternal(bool requireDisconnected, bool unpair = false) {
    // State may have changed after the timeout check earlier in this loop.
    // A power-off (unpair) request is allowed from any mode.
    if (s_sleeping ||
        (!unpair && (currentMode != MODE_IDLE || isCalibrating() ||
                     (requireDisconnected && bluetoothIsConnected())))) {
        return;
    }

    s_sleeping = true;
    logEvent("SLEEP", unpair ? "power_off_unpair" : "enter_idle_then_off");

    // Stops training/therapy and cancels calibration as side effects.
    if (currentMode != MODE_IDLE) {
        setDeviceMode(MODE_IDLE);
    }
    if (isCalibrating()) {
        calibrationRequestCancel();
    }

    motorCancelFeedback();
    motorSetDuty(0);
    if (unpair) {
        bluetoothUnpairAndShutdown();
    } else {
        bluetoothPrepareForSleep();
    }
    sleepPostureSensor();

    blinkSleepLed(true, false, false);
    blinkSleepLed(false, false, true);
    blinkSleepLed(false, true, false);
    hardReleaseSleepLeds();

    hardReleaseMotor();

    // Persist any just-ended session before the chip powers down.
    sessionStatsFlushNow();
    session_log_flush_now();

    // The button is still held when power-off triggers. SENSE_LOW would wake
    // the chip instantly, so wait for release (bounded) before System OFF.
    {
        const uint32_t t0 = millis();
        while (digitalRead(PIN_BUTTON) == LOW && (millis() - t0) < 10000UL) {
            delay(10);
        }
        delay(100);  // debounce the release
    }

    // Next wake must be a deliberate hold, not a tap.
    writePowerOffMarker();

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

void enterSleepMode() {
    enterSleepModeInternal(false);
}

void powerOffDevice() {
    enterSleepModeInternal(false, true);
}

void powerOnGate() {
    // Hold the motor off from the very first instruction (wake reset puts the
    // pin back to a floating input until motorSetup()).
    pinMode(PIN_MOTOR, OUTPUT);
    digitalWrite(PIN_MOTOR, LOW);

    // Only gate boots that follow a user power-off; flashing/reset boot normally.
    InternalFS.begin();
    if (!InternalFS.exists(kPowerOffMarkerPath)) {
        return;
    }

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    const uint32_t t0 = millis();
    while (digitalRead(PIN_BUTTON) == LOW) {
        if ((millis() - t0) >= kPowerOnHoldMs) {
            // Held long enough: power on for real.
            InternalFS.remove(kPowerOffMarkerPath);
            // Wait for release so the hold isn't also read as a click/hold.
            while (digitalRead(PIN_BUTTON) == LOW) {
                delay(10);
            }
            delay(100);
            return;
        }
        delay(10);
    }
    // Tap (or spurious wake): stay off. Marker stays so the next wake is gated too.
    systemOff(PIN_BUTTON, LOW);
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
        enterSleepModeInternal(true);
    }
}
