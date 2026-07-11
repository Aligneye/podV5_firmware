# RTT Debugger Module Bug Audit

## Scope

Reviewed `src/rtt_debugger.cpp`, `include/rtt_debugger.h`, all `logPacket`/`logEvent`/`rttDebuggerPrintBlePacket` call sites, RTT flags in `include/config.h`, and the installed RTT Stream/SEGGER implementation.

## Summary

- Confirmed findings: **2** — 2 medium
- Concurrency risks: **1** — medium
- Intentional note: `rttDebuggerLoop()` is currently an empty hook; this report does not count that alone as a defect because the current header explicitly says to keep non-BLE output disabled.

## Confirmed findings

### 1. `logPacket()` and `logEvent()` silently discard every event

- **Severity:** Medium
- **Location:** `include/rtt_debugger.h:24`, `include/rtt_debugger.h:29`
- **Evidence / trigger:** Both public helpers cast their arguments to void and return. Boot/reset cause, storage load/reset, calibration failures and phase transitions, sleep entry, inactivity resets, and mode/DFU events all call these helpers expecting diagnostics, but none can produce output.
- **Impact:** Important field failures have no RTT evidence. In particular reset-cause and storage/calibration diagnostics disappear even when developers believe the documented channel is enabled, materially slowing diagnosis and hiding failure order.
- **Suggested fix:** Implement bounded formatted writes behind compile-time channel flags, or remove the API and convert callers to an explicit enabled logger so a call cannot misleadingly compile into a silent no-op.

### 2. The BLE RTT flags are ignored while all BLE payloads are logged unconditionally

- **Severity:** Medium
- **Location:** `src/rtt_debugger.cpp:13`, `src/rtt_debugger.cpp:15`, `include/config.h:59`, `include/config.h:63`; unconditional call sites: `src/bluetooth.cpp:91`, `src/bluetooth.cpp:1208`
- **Evidence / trigger:** `ALIGN_RTT_JSON_LOG` and `ALIGN_RTT_BLE_RX_LOG` both default to 0, but neither macro is referenced by the debugger implementation. Every outgoing packet, including 150 ms live telemetry, and every incoming app command is mirrored regardless of configuration.
- **Impact:** Release builds pay continuous formatting/RTT traffic cost and expose command/profile/time data on the debug port, while enabling/disabling the documented flags has no effect. The mismatch makes build configuration untrustworthy.
- **Suggested fix:** Gate TX JSON and RX traffic independently with the advertised flags, define their exact semantics in one header, and compile the calls out when disabled.

## Concurrency risks

### 3. RX and TX log lines can interleave across tasks

- **Severity:** Medium risk
- **Location:** `src/rtt_debugger.cpp:15`, `src/rtt_debugger.cpp:16`, `src/rtt_debugger.cpp:17`, `src/rtt_debugger.cpp:18`
- **Evidence / trigger:** A line is emitted through four separate `Print` operations. BLE RX callbacks run on the Adafruit callback task while TX logging runs from the Arduino loop task. The bundled SEGGER RTT configuration has empty lock macros, so an RX command arriving during a TX line can interleave prefixes, directions, and payloads.
- **Impact:** Logs can become syntactically invalid or falsely associate a payload with the wrong direction, undermining the main purpose of the debugger. Exact frequency depends on traffic timing.
- **Suggested fix:** Build the complete line in a bounded local buffer and serialize one write with a mutex/critical section, or route both RX and TX records through a single logger queue consumed by one task.
