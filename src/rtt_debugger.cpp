#include "rtt_debugger.h"
#include "bluetooth.h"
#include "calibration.h"
#include "sleep.h"
#include "motor.h"
#include "therapy.h"
#include "training.h"
#include <RTTStream.h>

void rttDebuggerLoop() {
}

void rttDebuggerPrintBlePacket(const char* direction, const char* payload) {
    if (!direction || !payload) return;
    rtt.print("[BLE ");
    rtt.print(direction);
    rtt.print("] ");
    rtt.println(payload);
}
