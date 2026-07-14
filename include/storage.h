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
void storageCommitBatchDeferred();
void storageLoop();

bool saveTrainingDelay(TrainingDelay delay);
TrainingDelay loadTrainingDelay();

uint8_t storageLoadTherapySubMode();
bool    storageSaveTherapySubMode(uint8_t idx);

bool storageLoadProfiles(OrientationProfile* profiles, uint8_t* count);
bool storageSaveProfiles(const OrientationProfile* profiles, uint8_t count);
int8_t storageLoadActiveProfileIndex();
bool storageSaveActiveProfileIndex(int8_t index);
uint32_t storageLoadDefaultProfileId();
bool storageSaveDefaultProfileId(uint32_t id);
uint32_t storageLoadNextProfileId();
bool storageSaveNextProfileId(uint32_t id);
uint8_t storageLoadNextProfileOverwriteIndex();
bool storageSaveNextProfileOverwriteIndex(uint8_t index);

bool storageLoadCalibration(float* y, float* z);
bool storageSaveCalibration(float y, float z);
void storageFactoryReset();


