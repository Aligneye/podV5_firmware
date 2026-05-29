#pragma once

#include <Arduino.h>

enum TrainingDelay : uint8_t;

void storageSetup();

void saveTrainingDelay(TrainingDelay delay);
TrainingDelay loadTrainingDelay();

uint8_t storageLoadTherapySubMode();
void    storageSaveTherapySubMode(uint8_t idx);

bool storageLoadCalibration(float* y, float* z);
void storageSaveCalibration(float y, float z);
