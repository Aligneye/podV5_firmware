#include "rtt_debugger.h"
#include "bluetooth.h"
#include "calibration.h"
#include "training.h"
#include <RTTStream.h>

void rttDebuggerLoop() {
    static unsigned long lastStatsMs = 0;
    const unsigned long now = millis();
    if ((now - lastStatsMs) < 1000UL) {
        return;
    }
    lastStatsMs = now;

    const OrientationProfile* active = getActiveProfile();
    const char* profileName = active ? active->name : "DEFAULT";
    const uint8_t profileCount = getProfileCount();
    const uint8_t batteryPct = bluetoothGetBatteryPercentage();

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"raw_x\":%.2f,\"raw_y\":%.2f,\"raw_z\":%.2f,\"angle\":%.1f,"
             "\"active_profile\":\"%s\",\"profile_count\":%u,\"battery_pct\":%u}",
             rawX, rawY, rawZ, currentAngle, profileName, (unsigned)profileCount, (unsigned)batteryPct);
    rtt.print("[RTT DBG] ");
    rtt.println(payload);
}

void rttDebuggerPrintBlePacket(const char* direction, const char* payload) {
    if (!direction || !payload) return;
    rtt.print("[BLE ");
    rtt.print(direction);
    rtt.print("] ");
    rtt.println(payload);
}
