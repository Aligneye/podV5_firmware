#include "autoOff.h"

#include "button.h"
#include "bluetooth.h"
#include "training.h"
#include "motor.h"
#include "device_time.h"
#include "calibration.h"
#include "config.h"

#include <limits.h>
#include <math.h>

const unsigned long AUTO_OFF_DURATION_MS = 2UL * 60UL * 1000UL;
const unsigned long MOTION_DEBOUNCE_MS = 2000UL;
const float ANGLE_CHANGE_THRESHOLD = 3.0f;

static unsigned long lastActivityTime = 0;
static unsigned long motionStartTime = 0;
static bool wasMoving = false;
static float lastAngle = 0.0f;
static bool angleInitialized = false;

static inline void releaseBuiltInLedsForSleep() {
    pinMode(PIN_LED_RED, INPUT);
    pinMode(PIN_LED_GREEN, INPUT);
    pinMode(PIN_LED_BLUE, INPUT);
}

static unsigned long safeTimeDiff(unsigned long current, unsigned long previous) {
    if (current >= previous) {
        return current - previous;
    }
    return (ULONG_MAX - previous) + current + 1UL;
}

void initAutoOff() {
    lastActivityTime = millis();
    motionStartTime = 0;
    wasMoving = false;
    lastAngle = 0.0f;
    angleInitialized = false;
}

void resetAutoOffTimer() {
    lastActivityTime = millis();
    lastAngle = currentAngle;
    angleInitialized = true;
}

void autoOffMarkActivity() {
    resetAutoOffTimer();
}

void checkAutoOff() {
    if (!deviceOn || currentMode == MODE_OFF || isCalibrating()) {
        return;
    }

    bool isActive = false;
    const unsigned long now = millis();

    if (!angleInitialized) {
        lastAngle = currentAngle;
        angleInitialized = true;
    } else {
        float angleDiff = fabsf(currentAngle - lastAngle);
        if (angleDiff >= ANGLE_CHANGE_THRESHOLD) {
            isActive = true;
            lastAngle = currentAngle;
        }
    }

    bool currentlyMoving = isDeviceMoving();
    if (currentlyMoving) {
        if (!wasMoving) {
            motionStartTime = now;
            wasMoving = true;
        } else if (safeTimeDiff(now, motionStartTime) >= MOTION_DEBOUNCE_MS) {
            isActive = true;
        }
    } else {
        wasMoving = false;
        motionStartTime = 0;
    }

    if (isActive || bluetoothIsConnected()) {
        lastActivityTime = now;
    }

    if (safeTimeDiff(now, lastActivityTime) >= AUTO_OFF_DURATION_MS) {
        powerOff();
    }
}

void powerOff() {
    persistDeviceTime();

    bluetoothStopAdvertising();

    motorSetDuty(0);
    sleepPostureSensor();
    releaseBuiltInLedsForSleep();

    currentMode = MODE_OFF;
    deviceOn = false;
    lastActivityTime = millis();
}
