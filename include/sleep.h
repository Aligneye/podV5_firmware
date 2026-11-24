#pragma once

#include <Arduino.h>

void sleepTimerSetup();
void sleepTimerLoop();
void enterSleepMode();
/** Hold-to-power-off from any mode: stop everything, unpair BLE, System OFF. */
void powerOffDevice();
/** Call first in setup(): after a System OFF wake, require a hold to power on. */
void powerOnGate();
