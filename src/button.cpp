#include "button.h"
#include "bluetooth.h"
#include "calibration.h"
#include "motor.h"
#include "storage.h"
#include "therapy.h"
#include "device_time.h"
#include "training.h"
#include <OneButton.h>

#ifdef DEBUG_LOGGING
#define DEBUG_PRINT(x) rtt.print(x)
#define DEBUG_PRINTLN(...) rtt.println(__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(...)
#endif

extern RTTStream rtt;

// ── Name arrays ────────────────────────────────────────────────────────────
const char *modeNames[] = {"Training Mode", "Therapy Mode", "OFF Mode"};
const char *trainingSubModes[] = {"Instant", "Delayed", "No alerts"};
const char *therapySubModes[] = {"10 min", "20 min", "30 min"};

// ── State definitions ──────────────────────────────────────────────────────
bool deviceOn = true;
Mode currentMode = MODE_TRAINING;
TrainingAlertStyle trainingSubModeIndex = TrainingAlertStyle::Instant;
uint8_t therapySubModeIndex = 0;
unsigned long lastModeChangeMs = 0;

// OneButton instance: active LOW, internal pull-up enabled
static OneButton btn(PIN_BUTTON, true, true);

static void playButtonPressHaptic() {
  if (calibrationMotorActive())
    return;
  // Use a low duty cycle (80) to prevent brownout reset while providing haptic
  // feedback
  motorOverrideDuty(150, 125);
}

static void printCurrentMode() {
  DEBUG_PRINT("Mode: ");
  DEBUG_PRINTLN(modeNames[currentMode]);
  if (currentMode == MODE_THERAPY) {
    DEBUG_PRINT("Therapy Sub-Mode: ");
    DEBUG_PRINTLN(therapySubModes[therapySubModeIndex]);
  }
}

static void handleSingleClick() {
  // Cycle modes: Training -> Therapy -> Off -> Training
  currentMode = (Mode)((currentMode + 1) % MODE_COUNT);
  lastModeChangeMs = millis();

  // Stop any active tasks immediately (vibration and therapy)
  if (therapyIsRunning()) {
    therapyStop(false);
  }
  motorSetDuty(0);

  printCurrentMode();
}

static void handleDoubleClick() {
  DEBUG_PRINTLN("Double click");

  // Play haptic feedback for the double click event
  playButtonPressHaptic();

  switch (currentMode) {
  case MODE_TRAINING:
    trainingSubModeIndex = static_cast<TrainingAlertStyle>((static_cast<uint8_t>(trainingSubModeIndex) + 1) % TRAINING_SUBMODE_COUNT);
    DEBUG_PRINT("Training Sub-Mode: ");
    DEBUG_PRINTLN(trainingSubModes[static_cast<uint8_t>(trainingSubModeIndex)]);
    break;

  case MODE_THERAPY:
    // Stop session but stay in Therapy mode, then apply new duration
    therapyStop(false);
    therapySubModeIndex = (therapySubModeIndex + 1) % THERAPY_SUBMODE_COUNT;

    DEBUG_PRINT("Therapy Sub-Mode changed: ");
    DEBUG_PRINTLN(therapySubModes[therapySubModeIndex]);
    therapyStart();
    break;

  default:
    break;
  }
}

static void handleHold() {
  DEBUG_PRINTLN("Hold");
  if (currentMode == MODE_OFF) {
    DEBUG_PRINTLN("Hold ignored in OFF mode");
    return;
  }
  if (isCalibrating()) {
    calibrationRequestCancel();
  } else {
    calibrationRequestStart();
  }
}

void buttonSetup() {
  // Bind callbacks to OneButton
  btn.attachPress(playButtonPressHaptic);
  btn.attachClick(handleSingleClick);
  btn.attachDoubleClick(handleDoubleClick);
  btn.attachLongPressStart(handleHold);

  // Set long press duration (matching HOLD_MS)
  btn.setPressMs(HOLD_MS);

  therapySubModeIndex = storageLoadTherapySubMode();

  DEBUG_PRINTLN("Device ON");
  currentMode = MODE_TRAINING;
  printCurrentMode();
}

static bool offPrinted = true;
static bool trainingStarted = true;

void buttonLoop() {
  btn.tick();

  bool isTransitioning = (millis() - lastModeChangeMs < 1000);

  if (!isTransitioning) {
    if (currentMode == MODE_OFF && !offPrinted) {
      rtt.println("off");
      offPrinted = true;
    }

    if (currentMode == MODE_TRAINING && !trainingStarted) {
      deviceOn = true;
      bluetoothStartAdvertising();
      trainingStarted = true;
    }

    if (currentMode == MODE_THERAPY && !therapyIsRunning() && !isCalibrating()) {
      DEBUG_PRINTLN("Therapy auto-start (mode is Therapy)");
      deviceOn = true;
      therapyStart();
    }
  } else {
    if (currentMode == MODE_OFF) {
      offPrinted = false;
    } else if (currentMode == MODE_TRAINING) {
      trainingStarted = false;
    }
  }
}
