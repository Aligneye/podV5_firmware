#pragma once

#include <Arduino.h>

void stepCountInit();
void stepCountProcessSample(float ax, float ay, float az, uint32_t nowMs);
uint32_t stepCountGetTotal();
void stepCountReset();
