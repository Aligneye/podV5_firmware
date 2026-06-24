#pragma once

#include <Arduino.h>

enum TrainingDelay : uint8_t;

void storageSetup();

void saveTrainingDelay(TrainingDelay delay);
TrainingDelay loadTrainingDelay();

uint8_t storageLoadTherapySubMode();
void    storageSaveTherapySubMode(uint8_t idx);

struct OrientationProfile;
bool storageLoadProfiles(OrientationProfile* profiles, uint8_t* count);
void storageSaveProfiles(const OrientationProfile* profiles, uint8_t count);
int8_t storageLoadActiveProfileIndex();
void storageSaveActiveProfileIndex(int8_t index);
uint32_t storageLoadDefaultProfileId();
void storageSaveDefaultProfileId(uint32_t id);
uint32_t storageLoadNextProfileId();
void storageSaveNextProfileId(uint32_t id);
uint8_t storageLoadNextProfileOverwriteIndex();
void storageSaveNextProfileOverwriteIndex(uint8_t index);

bool storageLoadCalibration(float* y, float* z);
void storageSaveCalibration(float y, float z);


