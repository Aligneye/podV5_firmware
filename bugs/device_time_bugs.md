# Device Time Bug Audit

## Scope

Reviewed `src/device_time.cpp`, `include/device_time.h`, filesystem and RTC behavior, setup/main-loop scheduling, BLE time input, sleep/shutdown, session timestamp consumers, step-count consumers, and time documentation. No source code was changed.

## Summary

- Confirmed defects: 3
- Conditional risks / hardening notes: 4
- Highest-risk issue: periodic persistence and forced pre-sleep persistence are both bypassed while the device remains Idle.

## Confirmed defects

### 1. Time maintenance never runs in Idle, and System OFF does not force a final save

- **Severity:** High
- **Source:** `src/main.cpp:53`, `src/main.cpp:54`, `src/main.cpp:61`, `src/device_time.cpp:242`, `src/device_time.cpp:247`, `src/sleep.cpp:58`, `src/sleep.cpp:84`, `src/sleep.cpp:85`, `src/device_time.cpp:329`
- **Evidence / trigger:** `main.loop()` returns for `MODE_IDLE` before `maintainDeviceTime()`. The inactivity path can then enter System OFF without calling `persistDeviceTime()`, and repository-wide search finds no caller for that force-save API. A time sync itself is saved, but no later snapshot is made during a long Idle period.
- **Impact:** Normal inactivity reaches System OFF after roughly five Idle minutes, so the snapshot carries several minutes of avoidable on-time staleness in addition to the powered-off interval that this RTC design cannot reconstruct. Restored `TIME_STALE` therefore starts behind and can give sessions/day resets the wrong time until the app syncs again.
- **Suggested fix:** Run `maintainDeviceTime()` before the Idle early return and call a checked `persistDeviceTime()` immediately before System OFF/DFU/reset paths. Keep the stale status after reboot because powered-off elapsed time is still unknowable without a retained RTC.

### 2. BLE cannot set epochs after the signed-32-bit 2038 boundary despite the time API accepting through 2100

- **Severity:** Low (future-dated but deterministic)
- **Source:** `src/device_time.cpp:31`, `src/device_time.cpp:32`, `src/device_time.cpp:256`, `src/device_time.cpp:260`, `src/bluetooth.cpp:791`, `src/bluetooth.cpp:794`, `src/bluetooth.cpp:795`, `src/bluetooth.cpp:796`
- **Evidence / trigger:** The BLE parser uses `String::toInt()` into `long`, which is signed 32-bit on this ARM target, and requires the result to be positive. Unix epochs after 2,147,483,647 (2038-01-19) cannot survive that parse, while `setDeviceTime(uint32_t)` explicitly accepts values up to 4,102,444,800.
- **Impact:** The external sync path stops updating device time in 2038, decades before the advertised API range ends.
- **Suggested fix:** Parse decimal input with checked unsigned 64-bit conversion, reject overflow/trailing characters explicitly, range-check against the device-time limits, and then cast to `uint32_t`. Return an error acknowledgement for invalid input.

### 3. The “atomic” time-file update removes the last valid snapshot before rename

- **Severity:** Medium
- **Source:** `src/device_time.cpp:65`, `src/device_time.cpp:76`, `src/device_time.cpp:84`, `src/device_time.cpp:85`, `src/device_time.cpp:86`, `src/device_time.cpp:102`
- **Evidence / trigger:** After writing and flushing `/devtime.tmp`, the code deletes `/devtime.dat` before attempting to rename the temp file. Power loss or rename/open failure after deletion leaves no primary snapshot, and `loadPersistedTime()` never attempts recovery from the temp file.
- **Impact:** A reset during a routine time checkpoint can turn a previously usable stale clock into `TIME_UNKNOWN` and thereby remove timestamps/event detail from later sessions.
- **Suggested fix:** Replace without pre-deleting where LittleFS supports atomic rename-overwrite, or maintain two generation-tagged validated slots. On boot, recover the newest valid primary/temp candidate.

## Conditional risks and hardening notes

### 4. Every accepted time-sync command forces a flash rewrite and bypasses wear throttling

- **Severity:** Medium (endurance risk)
- **Source:** `src/device_time.cpp:122`, `src/device_time.cpp:126`, `src/device_time.cpp:256`, `src/device_time.cpp:269`, `src/bluetooth.cpp:791`
- **Evidence / trigger:** `setDeviceTime()` always calls `savePersistedTime(epochSeconds, true)`. The `force` argument bypasses `MIN_PERSIST_DELTA_SECONDS`, so repeated app sync messages or reconnect-time syncs rewrite the file even when the epoch changed only slightly.
- **Impact:** A chatty or malfunctioning client can cause unnecessary erase/program cycles and shorten internal-flash life.
- **Suggested fix:** Persist immediately only when no valid snapshot exists or when the change exceeds a policy threshold; otherwise coalesce through normal maintenance. Rate-limit externally triggered forced writes.

### 5. Persisted time has no checksum and accepts plausible corruption

- **Severity:** Medium (data-integrity risk)
- **Source:** `src/device_time.cpp:56`, `src/device_time.cpp:65`, `src/device_time.cpp:107`, `src/device_time.cpp:109`, `src/device_time.cpp:111`
- **Evidence / trigger:** `PersistedTime` contains magic/version and reserved words but no CRC. Loading accepts any epoch within 2024–2100 when magic/version happen to remain valid.
- **Impact:** A bit error can silently restore a believable but incorrect date, which is more difficult to detect than `TIME_UNKNOWN` and contaminates session keys/timestamps.
- **Suggested fix:** Store and verify a CRC over the versioned payload, plus a generation counter if redundant slots are used.

### 6. Tick reads can briefly move backward at each RTC2 hardware overflow

- **Severity:** Low (rare concurrency risk)
- **Source:** `src/device_time.cpp:146`, `src/device_time.cpp:150`, `src/device_time.cpp:183`, `src/device_time.cpp:188`, `src/device_time.cpp:190`, `src/device_time.cpp:193`, `src/device_time.cpp:194`
- **Evidence / trigger:** RTC2 wraps about every 24.3 days at 8 Hz. If the hardware counter has wrapped and set `EVENTS_OVRFLW` but its ISR is still pending/masked, both software-overflow reads can agree on the old value while the counter read is already small. The double-read loop therefore accepts a value roughly one hardware period behind.
- **Impact:** A time/timestamp read in that narrow window can jump backward until the ISR increments `g_rtc2Overflows`.
- **Suggested fix:** Include the pending overflow event in the read algorithm (with the hardware-recommended counter/event ordering), or read inside a short critical section and compensate when the event is set and the counter is in the post-wrap range.

### 7. The timezone offset is volatile even though local-time APIs are public

- **Severity:** Low (product-behavior risk)
- **Source:** `src/device_time.cpp:54`, `src/device_time.cpp:368`, `src/device_time.cpp:372`, `src/device_time.cpp:375`, `src/device_time.cpp:379`
- **Evidence / trigger:** `g_tzOffsetSeconds` always initializes to zero and is not part of `PersistedTime`. Reboot resets local formatting to UTC until the app sends the timezone again.
- **Impact:** Local timestamps and any future local-day features can change behavior across reboot or while disconnected from the app.
- **Suggested fix:** Either persist the validated timezone in settings or explicitly define it as connection-scoped and avoid using it for durable calendar semantics.
