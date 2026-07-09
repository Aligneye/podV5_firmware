#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

void bluetoothSetup();
void bluetoothLoop();

void bluetoothStartAdvertising();
void bluetoothStopAdvertising();
void bluetoothPrepareForSleep();
bool bluetoothIsConnected();
void bluetoothUnlockForPairing();

/** Safe from BLE RX callback: only sets a deferred flag (see calibration). */
void bluetoothRequestCalibrationStart();
void bluetoothRequestBatteryStatusBlink();
void bluetoothNotifyStateChanged();
void notifyCalibrationStatus(const char* calibResult, const char* complete);
void notifyCalibrationComplete(bool success, uint32_t profileId, const char* name, uint8_t slot, uint16_t quality, uint16_t sampleCount, const char* reason, float refX = 0.0f, float refY = 0.0f, float refZ = 0.0f, uint16_t passedSamples = 0);
void notifyDfuStatus(const char* status);
void notifyDeviceInfo();
void notifyTherapyComplete(uint32_t elapsedSeconds);
void bluetoothRequestFactoryReset();
uint8_t bluetoothGetBatteryPercentage();
bool bluetoothIsMotorActive();

/** Notify BLE layer that a new session was stored. */
void notifyNewSessionStored();
