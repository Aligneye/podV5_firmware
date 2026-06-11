#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

// ── Profile Struct ──────────────────────────────────────────────────────────
struct OrientationProfile {
    char name[16];
    float refX;
    float refY;
    float refZ;
    uint32_t createdAt;
};

// ── Profile Storage APIs ────────────────────────────────────────────────────
bool addCalibrationProfile(const char* name);
bool addNextCalibrationProfile();
bool deleteCalibrationProfile(uint8_t index);
void clearCalibrationProfiles();
uint8_t getProfileCount();
const OrientationProfile* getProfile(uint8_t index);

// ── Active Profile APIs ─────────────────────────────────────────────────────
int getActiveProfileIndex();
const OrientationProfile* getActiveProfile();
bool selectCalibrationProfile(uint8_t index);
void selectDefaultCalibrationProfile();
bool detectCurrentOrientationProfile();

// ── Helper / Update API for training ───────────────────────────────────────
void updateActiveProfileReference(float refX, float refY, float refZ);
void addOrUpdateProfile0(float refX, float refY, float refZ);
void initProfiles();

// ── Temporary Calibration Results retrieval ─────────────────────────────────
float getLastCalibratedX();
float getLastCalibratedY();
float getLastCalibratedZ();
bool isLastCalibrationValid();

// ── Lifecycle ───────────────────────────────────────────────────────────────
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
