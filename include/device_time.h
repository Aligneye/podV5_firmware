#pragma once

#include <Arduino.h>

enum DeviceTimeStatus : uint8_t {
    TIME_UNKNOWN = 0,
    TIME_STALE   = 1,
    TIME_FRESH   = 2
};

void initDeviceTime();
void maintainDeviceTime();

void setDeviceTime(uint32_t epochSeconds);
uint32_t getDeviceTime();

uint64_t getDeviceTicks();
uint32_t ticksToEpoch(uint64_t ticks);

bool isDeviceTimeSynced();
DeviceTimeStatus getDeviceTimeStatus();

uint32_t getDeviceUptimeSeconds();
uint32_t getSecondsSinceSync();

void persistDeviceTime();

void formatEpochUTC(uint32_t epochSeconds, char *out, size_t outLen);
void formatEpochISO(uint32_t epochSeconds, char *out, size_t outLen);
void formatEpochLocal(uint32_t epochSeconds, char *out, size_t outLen);

void setDeviceTZOffset(int32_t offsetSeconds);
int32_t getDeviceTZOffset();
