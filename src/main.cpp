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
#include "autoOff.h"
#include "device_time.h"
#include "motor.h"
#include "session_stats.h"

RTTStream rtt;

static void printResetReason() {
    uint32_t reason = NRF_POWER->RESETREAS;
    rtt.print("Reset reason: 0x");
    rtt.println(reason, HEX);
    if (reason == 0) {
        rtt.println("Reset flags: none latched");
    } else {
        rtt.print("Reset flags:");
        if (reason & POWER_RESETREAS_RESETPIN_Msk) rtt.print(" RESETPIN");
        if (reason & POWER_RESETREAS_DOG_Msk)      rtt.print(" WATCHDOG");
        if (reason & POWER_RESETREAS_SREQ_Msk)     rtt.print(" SOFT");
        if (reason & POWER_RESETREAS_LOCKUP_Msk)   rtt.print(" LOCKUP");
        if (reason & POWER_RESETREAS_OFF_Msk)      rtt.print(" SYSTEM_OFF_WAKE");
        if (reason & POWER_RESETREAS_LPCOMP_Msk)   rtt.print(" LPCOMP");
        if (reason & POWER_RESETREAS_DIF_Msk)      rtt.print(" DEBUG_IF");
        if (reason & POWER_RESETREAS_NFC_Msk)      rtt.print(" NFC");
        rtt.println();
    }
    NRF_POWER->RESETREAS = reason; // clear latched flags
}

void setup() {
    rtt.println("=== AlignEye Firmware V4 ===");
    printResetReason();
    storageSetup();
    initSessionStats();
    buttonSetup();
    therapySetup();
    trainingSetup();
    calibrationSetup();
    bluetoothSetup();
    initDeviceTime();
    initAutoOff();
}

void loop() {
    buttonLoop();
    therapyLoop();
    // Training first: posture motor is authoritative unless calibration idle-handler
    // applies short success/fail buzz afterward (see calibration.cpp).
    trainingLoop();
    calibrationLoop();
    bluetoothLoop();
    motorUpdate();
    maintainDeviceTime();
    checkAutoOff();
    updateSessionStats();
    maintainSessionStats();
}
