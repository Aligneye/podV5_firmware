#pragma once

#include <Arduino.h>

void motorSetup();
void motorSetDuty(uint8_t duty);  // 0 = off, 255 = full on
void motorUpdate();                // call frequently from loop()
void motorOverrideDuty(uint8_t duty, uint16_t durationMs); // temporary priority pulse
void motorStartCalmHaptic(uint16_t durationMs = 320);
void motorCancelCalmHaptic();
