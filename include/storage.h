#pragma once

#include <Arduino.h>
#include "calibration.h"

enum TrainingDelay : uint8_t;

void storageSetup();

// Defer flash writes so a multi-step update commits once instead of once per
// setter. Nestable; the outermost storageCommitBatch() performs the write and
// returns whether it succeeded. storageCommitBatchDeferred() instead hands the
// write to storageLoop() (called from the main loop), so the calling code
// path is never blocked behind the flash commit.
void storageBeginBatch();
bool storageCommitBatch();
// Returns a non-zero ticket when a durable write was scheduled. The ticket can
// be polled without touching flash, allowing callers to acknowledge the fast
// RAM update separately from the eventual durable commit.
uint32_t storageCommitBatchDeferred();
void storageLoop();
uint32_t storageGetLatestDeferredPersistTicket();
bool storageGetDeferredPersistResult(uint32_t ticket,
                                     bool* success,
                                     uint32_t* totalMs,
                                     uint32_t* flashMs);

bool saveTrainingDelay(TrainingDelay delay);
TrainingDelay loadTrainingDelay();

// Posture-angle (degrees) last chosen from the app; 0 = never set.
uint8_t storageLoadTrainingAngle();
bool    storageSaveTrainingAngle(uint8_t deg);

// Last therapy intensity (1-3) and duration (min) chosen by the user.
// Load returns false if nothing has been saved yet.
bool storageLoadTherapyParams(uint8_t* intensity, uint8_t* durationMin);
bool storageSaveTherapyParams(uint8_t intensity, uint8_t durationMin);

uint8_t storageLoadTherapySubMode();
bool    storageSaveTherapySubMode(uint8_t idx);

bool storageLoadProfiles(OrientationProfile* profiles, uint8_t* count);
bool storageSaveProfiles(const OrientationProfile* profiles, uint8_t count);
uint32_t storageLoadActiveProfileId();
bool storageSaveActiveProfileId(uint32_t id);
uint32_t storageLoadDefaultProfileId();
bool storageSaveDefaultProfileId(uint32_t id);
uint32_t storageLoadNextProfileId();
bool storageSaveNextProfileId(uint32_t id);

bool storageLoadCalibration(float* y, float* z);
bool storageSaveCalibration(float y, float z);
void storageFactoryReset();


