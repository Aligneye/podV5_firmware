# Main Entrypoint Bug Audit

## Scope

Reviewed `src/main.cpp`, setup/loop ordering, `include/config.h`, `include/version.h`, and the mode, session, device-time, inactivity, BLE, calibration, therapy, and training lifecycle calls affected by that ordering.

## Summary

- Confirmed findings: **2** — 1 medium, 1 low
- Consistency notes: **1** — low
- No source code was modified; the current target builds successfully.

## Confirmed findings

### 1. Device-time maintenance never runs while the device is idle

- **Severity:** Medium
- **Location:** `src/main.cpp:53`, `src/main.cpp:54`, `src/main.cpp:61`; maintenance behavior: `src/device_time.cpp:242`, `src/device_time.cpp:247`
- **Evidence / trigger:** `loop()` returns for every `MODE_IDLE` iteration before `maintainDeviceTime()`. After a BLE time sync, the initial epoch is persisted immediately, but the periodic five-minute refresh cannot run during idle. Idle is also the state that reaches system-off after five minutes.
- **Impact:** A device that syncs and then sits idle can enter system-off with only the old sync-time value stored. On wake/reset, restored stale time can be behind by the entire idle interval (and longer if sleep is entered another way), affecting profile/session timestamps.
- **Suggested fix:** Move `maintainDeviceTime()` before the mode/transition early return and explicitly persist immediately before system-off. Keep flash-rate limiting inside the device-time module.

### 2. A session crossing 30 seconds on the same iteration as a button mode change can be discarded

- **Severity:** Low
- **Location:** `src/main.cpp:47`, `src/main.cpp:62`; lifecycle evidence: `src/button.cpp:63`, `src/button.cpp:64`, `src/session_stats.cpp:286`, `src/session_stats.cpp:422`
- **Evidence / trigger:** Button handling is first and can call `trainingStop()`/`onTrainingEnded()` before `updateSessionStats()` runs at the end. Promotion is stored as a separate boolean set only by `updateSessionStats()` after elapsed time reaches 30 seconds. If the previous loop was just below 30 seconds and the stop click is processed just after the threshold, finalization still sees `trainingPromoted == false` and discards the record despite elapsed time being at least 30 seconds. The new idle mode then takes the early return, so promotion cannot be corrected.
- **Impact:** A valid threshold-length session can disappear at a narrow but real boundary, creating inconsistent behavior around exactly 30 seconds.
- **Suggested fix:** Make `onTrainingEnded()`/`onTherapyEnded()` decide promotion from elapsed time directly, or update promotion before processing stop-capable events. Do not rely on a periodically refreshed boolean for the final threshold decision.

## Consistency notes

### 3. The boot identity still labels V5 firmware as V4

- **Severity:** Low note
- **Location:** `src/main.cpp:33`, `include/version.h:4`, `include/version.h:5`
- **Evidence / trigger:** The boot event literal is `aligneye_firmware_v4`, while the configured model/hardware are `ALIGN_POD_V5` and `POD_V5`. The event is currently hidden by the no-op RTT logger, but it becomes visible as soon as logging is repaired.
- **Impact:** Manufacturing/field logs can identify the wrong hardware generation and lead technicians to use the wrong assumptions or artifacts.
- **Suggested fix:** Generate boot identity from `DEVICE_MODEL`, `HW_VERSION`, and `FW_VERSION` rather than maintaining a separate hard-coded generation string.
