#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

void bluetoothSetup();
void bluetoothLoop();

void bluetoothStartAdvertising();
void bluetoothStopAdvertising();
bool bluetoothIsConnected();
void bluetoothUnlockForPairing();

/** Safe from BLE RX callback: only sets a deferred flag (see calibration). */
void bluetoothRequestCalibrationStart();
void bluetoothRequestBatteryStatusBlink();
void notifyCalibrationStatus(bool started, const char* status, const char* profileName, float refX, float refY, float refZ);

/** Notify BLE layer that a new session was stored. */
void notifyNewSessionStored();
