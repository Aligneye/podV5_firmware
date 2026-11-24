#include "button.h"
#include "bluetooth.h"
#include "calibration.h"
#include "motor.h"
#include "storage.h"
#include "therapy.h"
#include "device_time.h"
#include "sleep.h"
#include "training.h"
#include <OneButton.h>
#include <utility/SoftwareTimer.h>
#include "rtt_debugger.h"

#ifdef DEBUG_LOGGING
#define DEBUG_PRINT(x) rtt.print(x)
#define DEBUG_PRINTLN(...) rtt.println(__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(...)
#endif

extern RTTStream rtt;

// ── Name arrays ────────────────────────────────────────────────────────────
const char *modeNames[] = {"Training Mode", "Therapy Mode", "Idle Mode", "DFU Mode"};
const char *trainingSubModes[] = {"Instant", "Delayed", "No alerts"};
const char *therapySubModes[] = {"10 min", "20 min", "30 min"};

// ── State definitions ──────────────────────────────────────────────────────
bool deviceOn = true;
Mode currentMode = MODE_IDLE;
TrainingAlertStyle trainingSubModeIndex = TrainingAlertStyle::Instant;
uint8_t therapySubModeIndex = 0; // Default therapy duration: 10 minutes.
unsigned long lastModeChangeMs = 0;
unsigned long lastModeChangeDelayMs = MODE_SWITCH_DELAY_MS;

static bool idlePrinted = false;
static bool trainingStarted = false;

// OneButton instance: active LOW, internal pull-up enabled
static OneButton btn(PIN_BUTTON, true, true);

void markSubModeChanged() {
  lastModeChangeMs = millis();
  lastModeChangeDelayMs = SUBMODE_SWITCH_DELAY_MS;
  bluetoothNotifyStateChanged();
}

void setDeviceMode(Mode newMode) {
  if (currentMode == newMode) {
    return;
  }

  Mode previousMode = currentMode;
  DEBUG_PRINT("setDeviceMode: ");
  DEBUG_PRINT(modeNames[previousMode]);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(modeNames[newMode]);

  // 1. Stop active tasks in the PREVIOUS mode immediately
  if (currentMode == MODE_TRAINING) {
    trainingStop();
  } else if (currentMode == MODE_THERAPY && therapyIsRunning()) {
    therapyStop(false);
  }

  if (isCalibrating()) {
    calibrationRequestCancel();
  }

  motorCancelFeedback();
  motorSetDuty(0);

  // 2. Transition to new mode
  currentMode = newMode;
  lastModeChangeMs = millis();
  // Mode changes are instant: no settle delay (session saving runs in the background).
  lastModeChangeDelayMs = 0UL;

  // Enrollment is intentionally an Idle-only operation. Leaving Idle closes
  // open advertising and stops the rapid red pairing indication.
  if (currentMode != MODE_IDLE) {
    bluetoothClosePairingWindow();
  }

  // 3. Reset state flags
  if (currentMode == MODE_IDLE) {
    idlePrinted = false;
  } else if (currentMode == MODE_TRAINING) {
    trainingStarted = false;
    if (previousMode == MODE_IDLE) {
      bluetoothRequestBatteryStatusBlink();
    }
  }
  if (currentMode == MODE_THERAPY && !isCalibrating()) {
    deviceOn = true;
    therapyStart();
  }
  bluetoothNotifyStateChanged();
}

static void handleSingleClick() {
  DEBUG_PRINTLN("Single click");
  if (isCalibrating()) {
    DEBUG_PRINTLN("Button click detected during calibration - canceling and shifting to Training");
  }

  // Single click cycles Idle -> Training -> Therapy -> Idle.
  // A click during calibration cancels it (setDeviceMode) and advances the cycle.
  Mode previousMode = currentMode;
  Mode nextMode = MODE_IDLE;
  if (currentMode == MODE_IDLE) {
    nextMode = MODE_TRAINING;
  } else if (currentMode == MODE_TRAINING) {
    nextMode = MODE_THERAPY;
  }
  setDeviceMode(nextMode);
  if (currentMode != previousMode) {
    bluetoothRequestClickLedGlow();
  }
}

static void handleDoubleClick() {
  DEBUG_PRINTLN("Double click");

  if (isCalibrating()) {
    DEBUG_PRINTLN("Button double-click detected during calibration - canceling");
    calibrationRequestCancel();
    return;
  }

  switch (currentMode) {
  case MODE_TRAINING:
    DEBUG_PRINTLN("Entering calibration from TRAINING mode");
    calibrationRequestStart();
    break;

  case MODE_THERAPY:
    // Change intensity without interrupting or restarting the therapy session.
    therapyIntensityLevel++;
    if (therapyIntensityLevel > 3) {
      therapyIntensityLevel = 1;
    }
    therapySaveParams();
    markSubModeChanged();
    bluetoothRequestBatteryStatusBlink();

    DEBUG_PRINT("Therapy intensity changed: ");
    DEBUG_PRINTLN(therapyIntensityLevel);
    break;

  case MODE_IDLE:
    DEBUG_PRINTLN("Entering calibration from IDLE mode");
    calibrationRequestStart();
    break;

  default:
    break;
  }
}

static void handleMultiClick(int clicks) {
  if (clicks != 3) {
    return;
  }

  DEBUG_PRINTLN("Triple click");

  if (isCalibrating()) {
    DEBUG_PRINTLN("Button triple-click detected during calibration - canceling");
    calibrationRequestCancel();
    return;
  }

  if (currentMode != MODE_IDLE) {
    DEBUG_PRINTLN("Triple click ignored outside IDLE mode");
    return;
  }

  enterSleepMode();
}

static void handleHold() {
  // Hold in any mode (idle/training/therapy/calibration): unpair BLE and
  // power the whole device off. Hold from off powers it on (see powerOnGate).
  DEBUG_PRINTLN("Hold -> unpair + power off");
  powerOffDevice();
}

// ── Loop-independent button sampling ───────────────────────────────────────
// The button used to be polled from loop(). Any slow iteration (flash write,
// BLE send, sensor read) made OneButton miss or mis-time presses: clicks were
// dropped and quick re-clicks turned into double/triple clicks. The button is
// now sampled every 5 ms from a FreeRTOS timer (priority above the loop task),
// and the detected gestures are queued and executed from buttonLoop(), so mode
// logic still runs only on the main loop.
enum ButtonEvent : uint8_t { BTN_EV_CLICK = 1, BTN_EV_DOUBLE, BTN_EV_MULTI, BTN_EV_HOLD };

static constexpr uint8_t kBtnQueueSize = 8;  // power of two
static volatile uint8_t s_btnQueue[kBtnQueueSize];
static volatile uint8_t s_btnQueueArg[kBtnQueueSize];
static volatile uint8_t s_btnHead = 0;  // written by timer task only
static volatile uint8_t s_btnTail = 0;  // written by loop only
static SoftwareTimer s_btnTimer;
static constexpr uint32_t kButtonSampleMs = 5;

static void pushButtonEvent(uint8_t ev, uint8_t arg = 0) {
  const uint8_t head = s_btnHead;
  const uint8_t next = (uint8_t)((head + 1u) & (kBtnQueueSize - 1u));
  if (next == s_btnTail) return;  // full: drop (cannot happen at human speed)
  s_btnQueue[head] = ev;
  s_btnQueueArg[head] = arg;
  __DMB();
  s_btnHead = next;
}

static void onBtnClick()  { pushButtonEvent(BTN_EV_CLICK); }
static void onBtnDouble() { pushButtonEvent(BTN_EV_DOUBLE); }
static void onBtnMulti()  {
  const int n = btn.getNumberClicks();
  pushButtonEvent(BTN_EV_MULTI, (uint8_t)(n > 255 ? 255 : n));
}
static void onBtnHold()   { pushButtonEvent(BTN_EV_HOLD); }

// Ignore a press that is already held when sampling starts (e.g. the tail of
// the power-on hold): the button must first be seen released for 50 ms.
static bool s_btnArmed = false;
static uint32_t s_btnReleasedSinceMs = 0;

static void buttonTimerCallback(TimerHandle_t) {
  // Runs in the FreeRTOS timer task: ONLY sample + detect. Gestures are queued
  // and executed by buttonLoop() on the main loop.
  if (!s_btnArmed) {
    const uint32_t now = millis();
    if (digitalRead(PIN_BUTTON) == LOW) {
      s_btnReleasedSinceMs = 0;
      return;
    }
    if (s_btnReleasedSinceMs == 0) s_btnReleasedSinceMs = now ? now : 1;
    if (now - s_btnReleasedSinceMs < 50UL) return;
    s_btnArmed = true;
  }
  btn.tick();
}

void buttonSetup() {
  // OneButton callbacks run in the timer task: they only queue events.
  btn.attachClick(onBtnClick);
  btn.attachDoubleClick(onBtnDouble);
  btn.attachMultiClick(onBtnMulti);
  btn.attachLongPressStart(onBtnHold);

  btn.setDebounceMs(DEBOUNCE_MS);
  btn.setClickMs(DOUBLE_CLICK_GAP_MS);
  // Set long press duration (matching HOLD_MS)
  btn.setPressMs(HOLD_MS);

  therapySubModeIndex = 0;

  DEBUG_PRINTLN("Device ON");
  currentMode = MODE_IDLE;
  trainingStarted = false;
  idlePrinted = false;

  s_btnTimer.begin(kButtonSampleMs, buttonTimerCallback);
  s_btnTimer.start();
}

void buttonLoop() {
  // Execute gestures detected by the 5 ms sampling timer.
  while (s_btnTail != s_btnHead) {
    const uint8_t tail = s_btnTail;
    const uint8_t ev = s_btnQueue[tail];
    const uint8_t arg = s_btnQueueArg[tail];
    __DMB();
    s_btnTail = (uint8_t)((tail + 1u) & (kBtnQueueSize - 1u));
    switch (ev) {
      case BTN_EV_CLICK:  handleSingleClick(); break;
      case BTN_EV_DOUBLE: handleDoubleClick(); break;
      case BTN_EV_MULTI:  handleMultiClick(arg); break;
      case BTN_EV_HOLD:   handleHold(); break;
      default: break;
    }
  }

  bool isTransitioning = (millis() - lastModeChangeMs < lastModeChangeDelayMs);

  if (!isTransitioning) {
    if (currentMode == MODE_IDLE && !idlePrinted) {
      logEvent("MODE", "idle");
      idlePrinted = true;
    }

    if (currentMode == MODE_TRAINING && !trainingStarted) {
      deviceOn = true;
      trainingStarted = true;
    }

    if (currentMode == MODE_THERAPY && !therapyIsRunning() && !isCalibrating()) {
      DEBUG_PRINTLN("Therapy auto-start (mode is Therapy)");
      deviceOn = true;
      therapyStart();
    }
  }
}

bool buttonInteractionPending() {
  return !btn.isIdle();
}
