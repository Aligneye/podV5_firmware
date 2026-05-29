#pragma once

#include <Arduino.h>

void initSessionStats();

void onTrainingStarted();
void onTrainingEnded();

void onTherapyStarted();
void onTherapyEnded();

void updateSessionStats();
void maintainSessionStats();

uint32_t getTrainingSessionNumber();
uint32_t getTrainingSessionDurationSec();
uint32_t getTrainingSessionBadPostureCount();
bool     isTrainingSessionActive();
bool     isTrainingActive();

uint32_t getTherapySessionNumber();
uint32_t getTherapySessionDurationSec();
bool     isTherapySessionActive();
bool     isTherapyActive();

uint32_t getActiveTrainingStartEpoch();
uint32_t getActiveTherapyStartEpoch();

uint32_t getLastTrainingStartEpoch();
uint32_t getLastTrainingEndEpoch();
uint32_t getLastTherapyStartEpoch();
uint32_t getLastTherapyEndEpoch();

void resetAllSessionCounters();
