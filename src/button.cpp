#include "button.h"
#include "bluetooth.h"
#include "calibration.h"
#include "motor.h"
#include "storage.h"
#include "therapy.h"
#include "device_time.h"
#include "training.h"
#include <OneButton.h>
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
Mode currentMode = MODE_TRAINING;
TrainingAlertStyle trainingSubModeIndex = TrainingAlertStyle::Instant;
uint8_t therapySubModeIndex = 0;
unsigned long lastModeChangeMs = 0;
unsigned long lastModeChangeDelayMs = MODE_SWITCH_DELAY_MS;

static bool idlePrinted = true;
static bool trainingStarted = true;

// OneButton instance: active LOW, internal pull-up enabled
static OneButton btn(PIN_BUTTON, true, true);

static void playButtonPressHaptic() {
  if (calibrationMotorActive())
    return;
  motorOverrideDuty(130, 70);
}

void markSubModeChanged() {
  lastModeChangeMs = millis();
  lastModeChangeDelayMs = SUBMODE_SWITCH_DELAY_MS;
}

void setDeviceMode(Mode newMode) {
  if (currentMode == newMode) {
    return;
  }

  Mode previousMode = currentMode;

  // 1. Stop active tasks in the PREVIOUS mode immediately
  if (currentMode == MODE_TRAINING) {
    trainingStop();
  } else if (currentMode == MODE_THERAPY) {
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
  lastModeChangeDelayMs = MODE_SWITCH_DELAY_MS;

  // 3. Reset state flags
  if (currentMode == MODE_IDLE) {
    idlePrinted = false;
  } else if (currentMode == MODE_TRAINING) {
    trainingStarted = false;
    if (previousMode == MODE_IDLE) {
      bluetoothRequestBatteryStatusBlink();
    }
  }
}

static void handleSingleClick() {
  if (isCalibrating()) {
    DEBUG_PRINTLN("Button click detected during calibration - canceling");
    calibrationRequestCancel();
    return;
  }

  // Cycle modes: Training -> Therapy -> Off -> Training
  Mode nextMode = (Mode)((currentMode + 1) % MODE_COUNT);
  setDeviceMode(nextMode);
}

static void handleDoubleClick() {
  DEBUG_PRINTLN("Double click");

  if (isCalibrating()) {
    DEBUG_PRINTLN("Button double-click detected during calibration - canceling");
    calibrationRequestCancel();
    return;
  }

  // Play haptic feedback for the double click event
  playButtonPressHaptic();

  switch (currentMode) {
  case MODE_TRAINING:
    trainingSubModeIndex = static_cast<TrainingAlertStyle>((static_cast<uint8_t>(trainingSubModeIndex) + 1) % TRAINING_SUBMODE_COUNT);
    markSubModeChanged();
    DEBUG_PRINT("Training Sub-Mode: ");
    DEBUG_PRINTLN(trainingSubModes[static_cast<uint8_t>(trainingSubModeIndex)]);
    break;

  case MODE_THERAPY:
    // Stop session but stay in Therapy mode, then apply new duration
    therapyStop(false);
    therapySubModeIndex = (therapySubModeIndex + 1) % THERAPY_SUBMODE_COUNT;
    markSubModeChanged();

    DEBUG_PRINT("Therapy Sub-Mode changed: ");
    DEBUG_PRINTLN(therapySubModes[therapySubModeIndex]);
    break;

  case MODE_IDLE:
    DEBUG_PRINTLN("Entering OTA DFU from IDLE mode");
    setDeviceMode((Mode)MODE_DFU);
    notifyDfuStatus("ARMED");
    logEvent("DFU", "entering_ota_bootloader");
    notifyDfuStatus("ENTERED");
    delay(50);
    enterOTADfu();
    break;

  default:
    break;
  }
}

static void handleMultiClick() {
  int clicks = btn.getNumberClicks();
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

  bluetoothUnlockForPairing();
}

static void handleHold() {
  DEBUG_PRINTLN("Hold");

  if (isCalibrating()) {
    DEBUG_PRINTLN("Button hold detected during calibration - ignoring");
    return;
  }

  setDeviceMode(MODE_IDLE);
  calibrationRequestStart();
}

void buttonSetup() {
  // Bind callbacks to OneButton
  btn.attachPress(playButtonPressHaptic);
  btn.attachClick(handleSingleClick);
  btn.attachDoubleClick(handleDoubleClick);
  btn.attachMultiClick(handleMultiClick);
  btn.attachLongPressStart(handleHold);

  btn.setDebounceMs(DEBOUNCE_MS);
  btn.setClickMs(DOUBLE_CLICK_GAP_MS);
  // Set long press duration (matching HOLD_MS)
  btn.setPressMs(HOLD_MS);

  therapySubModeIndex = storageLoadTherapySubMode();

  DEBUG_PRINTLN("Device ON");
  currentMode = MODE_TRAINING;
  trainingStarted = false;
  idlePrinted = true;
}

void buttonLoop() {
  btn.tick();

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
