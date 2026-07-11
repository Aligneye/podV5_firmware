#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"
#include "button.h"
#include "therapy.h"
#include "calibration.h"
#include "bluetooth.h"
#include "motor.h"
#include "training.h"

void inactivityTimerSetup();
void inactivityTimerLoop();
void inactivityTimerReset();
void inactivityTimerHoldoffAfterCalibration(uint32_t delayMs = 1500UL);

uint32_t inactivityTimerGetElapsedMs();
uint32_t inactivityTimerGetLastResetMs();
bool inactivityTimerHasActivity();
