#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

enum TherapyState {
    THERAPY_IDLE,
    THERAPY_RUNNING
};

enum TrainingDelay : uint8_t {
    TRAIN_DELAYED = 0,
    TRAIN_AUTOMATIC,
    TRAIN_INSTANT
};

enum TrainingSubMode : uint8_t {
    SUBMODE_DELAYED = 0,
    SUBMODE_AUTOMATIC,
    SUBMODE_INSTANT
};

extern TrainingDelay currentTrainingDelay;
extern unsigned long therapyDuration;
extern int currentPatternIndex;

void therapySetup();
void therapyLoop();

void therapyStart();
/** Stops motor + therapy state. If returnToTraining is false, leaves currentMode unchanged (e.g. sub-mode restart). */
void therapyStop(bool returnToTraining = true);

bool therapyIsRunning();
unsigned long therapyGetElapsedMs();
unsigned long therapyGetRemainingMs();
const char* therapyGetCurrentPatternName();
const char* therapyGetNextPatternName();

uint16_t getTherapyUniquePatternCount();
uint16_t getTherapyTotalPatternCount();
int getTherapyPatternSequence(uint8_t* out, uint8_t maxLen);
const char* getPatternNameByIndex(int idx);
