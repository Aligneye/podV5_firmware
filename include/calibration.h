#pragma once

#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"

struct CalibrationProfile {
    uint32_t id;
    char name[24];
    float refX;
    float refY;
    float refZ;
    uint32_t createdAtEpoch;
    uint16_t sampleCount;
    float stabilityScore;
    uint8_t valid;
    uint8_t reserved[3];
};

using OrientationProfile = CalibrationProfile;

struct CalibrationSettings {
    uint8_t version;
    uint8_t activeProfileIndex;
    uint8_t defaultProfileIndex;
    CalibrationProfile profiles[8];
    uint32_t crc;
};

bool addCalibrationProfile(const char* name);
bool addNextCalibrationProfile();
bool deleteCalibrationProfileById(uint32_t id);
bool renameCalibrationProfileById(uint32_t id, const char* name);
void clearCalibrationProfiles();
uint8_t getProfileCount();
const OrientationProfile* getProfile(uint8_t index);
const OrientationProfile* getProfileById(uint32_t id);
int getProfileIndexById(uint32_t id);
uint32_t getProfileIdByIndex(uint8_t index);

int getActiveProfileIndex();
const OrientationProfile* getActiveProfile();
bool selectCalibrationProfile(uint8_t index);
bool selectCalibrationProfileById(uint32_t id);
void selectDefaultCalibrationProfile();
bool detectCurrentOrientationProfile();
void setProfileDefaultById(uint32_t id);
uint32_t getDefaultProfileId();

void updateActiveProfileReference(float refX, float refY, float refZ);
void addOrUpdateProfile0(float refX, float refY, float refZ);
void initProfiles();

float getLastCalibratedX();
float getLastCalibratedY();
float getLastCalibratedZ();
bool isLastCalibrationValid();

void initCalibration();
void handleCalibration();
void requestCalibrationStart();
void requestCalibrationCancel();
void startCalibration();
void cancelCalibration();

const char* getCalibrationResult();
bool        isCalibrating();
uint32_t    getCalibrationElapsedMs();
uint32_t    getCalibrationTotalMs();
const char* getCalibrationPhase();

void calibrationSetup();
void calibrationLoop();
void calibrationRequestStart();
void calibrationRequestCancel();
void calibrationStart();
void calibrationStop();
bool calibrationIsActive();
bool calibrationMotorActive();
