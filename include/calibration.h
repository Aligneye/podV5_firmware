#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

// ── Lifecycle (matches ESP32 reference naming) ─────────────────────────────
void initCalibration();
void handleCalibration();

/** Deferred from BLE / ISR — processed inside `handleCalibration()`. */
void requestCalibrationStart();
void requestCalibrationCancel();

/** Immediate start if `CALIB_IDLE` (normally use `requestCalibrationStart`). */
void startCalibration();
void cancelCalibration();

// ── Queries ────────────────────────────────────────────────────────────────
const char* getCalibrationResult();
bool        isCalibrating();
uint32_t    getCalibrationElapsedMs();
uint32_t    getCalibrationTotalMs();
/** "IDLE", "GET_READY", or "HOLD_STILL" while running. */
const char* getCalibrationPhase();

// ── PlatformIO / legacy aliases (same behavior) ────────────────────────────
void calibrationSetup();
void calibrationLoop();
void calibrationRequestStart();
void calibrationRequestCancel();
void calibrationStart();
void calibrationStop();
bool calibrationIsActive();

/**
 * True while calibration logic should own motor output:
 * - active calibration phases
 * - post-result success/fail haptic windows
 */
bool calibrationMotorActive();
