#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

void trainingSetup();
void trainingLoop();

void trainingStart();
void trainingStop();

/** Call when user sets upright reference from accelerometer (Y,Z in m/s²). */
void setPostureOrigin(float y, float z);
void setPostureOrigin3D(float x, float y, float z);

void initPostureSensor(bool quick = false);
bool updatePostureAngle();

/** Read LIS3DH + LPF for calibration (any mode). Returns false if sensor missing. */
bool trainingSampleAccelForCalibration(void);
void trainingGetFilteredAccel(float* outY, float* outZ);
void sleepPostureSensor();
void wakePostureSensor();
bool isDeviceMoving();

bool     isTrainingSessionActive();
uint32_t getTrainingSessionNumber();
uint32_t getTrainingSessionDurationSec();
uint32_t getTrainingSessionBadPostureCount();
uint32_t getDeviceStepCount();

extern float rawX, rawY, rawZ;
extern float Y_ORIGIN, Z_ORIGIN;
extern float currentAngle;
extern bool  isBadPosture;
extern bool  sensorInitialized;
extern float kBadPostureDeg;
extern bool  s_bootProfileDetectionDone;

/** Short text for RTT / BLE (no Arduino String). */
extern char orientationText[16];
extern char directionText[16];
extern char postureText[96];
