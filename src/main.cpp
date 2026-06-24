#include <Arduino.h>
#include <RTTStream.h>
#include "nrf.h"
#include "config.h"
#include "storage.h"
#include "button.h"
#include "therapy.h"
#include "training.h"
#include "calibration.h"
#include "bluetooth.h"
#include "device_time.h"
#include "motor.h"
#include "session_stats.h"
#include "monitor_log.h"

RTTStream rtt;

static void printResetReason() {
    uint32_t reason = NRF_POWER->RESETREAS;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"t\":\"BOOT\",\"reset_reason\":\"0x%08lX\"}", (unsigned long)reason);
    logPacket("EVT", buf);
    if (reason == 0) {
        logEvent("BOOT", "reset_flags=none");
    } else {
        logEvent("BOOT", "reset_flags=present");
    }
    NRF_POWER->RESETREAS = reason; // clear latched flags
}

void setup() {
    logEvent("BOOT", "aligneye_firmware_v4");
    printResetReason();
    storageSetup();
    initSessionStats();
    buttonSetup();
    therapySetup();
    trainingSetup();
    calibrationSetup();
    bluetoothSetup();
    initDeviceTime();
}

void loop() {
    buttonLoop();
    motorUpdate();
    bluetoothLoop();
    if (currentMode == MODE_OFF || (millis() - lastModeChangeMs < lastModeChangeDelayMs)) {
        return;
    }
    therapyLoop();
    // Training first: posture motor is authoritative unless calibration idle-handler
    // applies short success/fail buzz afterward (see calibration.cpp).
    trainingLoop();
    calibrationLoop();
    maintainDeviceTime();
    updateSessionStats();
    maintainSessionStats();
}
