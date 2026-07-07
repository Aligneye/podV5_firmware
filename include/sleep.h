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

uint32_t inactivityTimerGetElapsedMs();
uint32_t inactivityTimerGetLastResetMs();
bool inactivityTimerHasActivity();
