#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

extern int therapyIntensityLevel;

void bluetoothSetup();
void bluetoothLoop();

void bluetoothStartAdvertising();
void bluetoothStopAdvertising();
void bluetoothPrepareForSleep();
bool bluetoothIsConnected();
bool bluetoothIsAuthorized();
bool bluetoothPairingWindowIsOpen();
void bluetoothUnlockForPairing();
/** Safe from any caller: bluetoothLoop() closes an open enrollment window. */
void bluetoothClosePairingWindow();

/** Safe from BLE RX callback: only sets a deferred flag (see calibration). */
void bluetoothRequestCalibrationStart();
void bluetoothRequestBatteryStatusBlink();
void bluetoothNotifyStateChanged();
void notifyCalibrationStatus(const char* calibResult, const char* complete);
void notifyCalibrationComplete(bool success, uint32_t profileId, const char* name, uint16_t quality, uint16_t sampleCount, const char* reason, float refX = 0.0f, float refY = 0.0f, float refZ = 0.0f, uint16_t passedSamples = 0);
void notifyCalibrationPersisted(uint32_t profileId, bool success, uint32_t totalMs, uint32_t flashMs);
void notifyDfuStatus(const char* status);
void notifyDeviceInfo();
void notifyTherapyComplete(uint32_t elapsedSeconds);
void bluetoothRequestFactoryReset();
uint8_t bluetoothGetBatteryPercentage();
bool bluetoothIsMotorActive();

/** Notify BLE layer that a new session was stored. */
void notifyNewSessionStored(uint32_t sessionId, uint8_t type);
