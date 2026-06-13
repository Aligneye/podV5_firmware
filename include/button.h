#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

// ── Power state ────────────────────────────────────────────────────────────
extern bool deviceOn;

// ── Modes ──────────────────────────────────────────────────────────────────
enum Mode {
    MODE_TRAINING = 0,
    MODE_THERAPY,
    MODE_OFF,
    MODE_COUNT
};

extern const char* modeNames[];

// ── Sub-modes ──────────────────────────────────────────────────────────────
#define TRAINING_SUBMODE_COUNT 3
#define THERAPY_SUBMODE_COUNT  3

extern const char* trainingSubModes[];
extern const char* therapySubModes[];

enum class TrainingAlertStyle : uint8_t {
    Instant = 0,
    Delayed = 1,
    NoAlerts = 2
};

// ── State ──────────────────────────────────────────────────────────────────
extern Mode               currentMode;
extern TrainingAlertStyle trainingSubModeIndex;
extern uint8_t therapySubModeIndex;
extern unsigned long lastModeChangeMs;
extern unsigned long lastModeChangeDelayMs;

// ── Public API ─────────────────────────────────────────────────────────────
void buttonSetup();
void buttonLoop();
void setDeviceMode(Mode newMode);
void markSubModeChanged();

