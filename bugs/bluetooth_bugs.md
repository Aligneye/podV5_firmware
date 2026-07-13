# Bluetooth Module Bug Audit

## Scope

Reviewed `src/bluetooth.cpp`, `include/bluetooth.h`, the installed Adafruit Bluefruit nRF52 implementation, the app contract in `private/aligneye_ble_protocol_docs.md`, and the profile, therapy, calibration, storage, session, device-time, motor, button, and sleep call sites that determine BLE behavior.

## Summary

- Confirmed findings: **13** — 7 high, 5 medium, 1 low
- Risks requiring runtime stress testing: **1** — medium
- The current firmware builds successfully for `nrf52832_custom`; these are runtime/protocol defects, not compile failures.

## Confirmed findings

### 1. The eight-profile response is truncated at 512 bytes

- **Severity:** High
- **Location:** `src/bluetooth.cpp:721`, `src/bluetooth.cpp:760`, `src/bluetooth.cpp:1239`
- **Evidence / trigger:** `sendProfileList()` deliberately builds a response in 800/1024-byte buffers, but the characteristic maximum is 512 bytes. A normal eight-profile response using names such as `Profile 1` is about 568 bytes; 23-character names can push it above 700 bytes. Bluefruit caps a notification to the characteristic maximum, so `GET_PROFILES` with eight profiles ends in the middle of the JSON object.
- **Impact:** The app's brace-depth framing never receives a complete `P` object. Profile refresh/selection can stop working precisely when the supported eight-slot store is full, and later packets can be consumed as part of the unterminated object until the app clears its buffer.
- **Suggested fix:** Paginate profiles into independently valid JSON packets, or define a compact multi-packet framing protocol with page/index fields. Do not rely on increasing the GATT value beyond its 512-byte maximum.

### 2. ~~The app's therapy-duration command is silently ignored~~ FIXED

- **Status:** Resolved
- **Fix:** Added JSON command `THERAPY_START` (accepts `therapy_intensity` 1-3 and `therapy_duration` 10/20/30). The new `startTherapyFromBle()` validates both fields, sets `therapyIntensityLevel` and `therapySubModeIndex`, persists to flash, then triggers mode switch. Legacy `THERAPY_INTENSITY` key-value parsing removed; legacy `MODE=THERAPY` semicolon path blocked. JSON `SET_MODE` with `"mode":"THERAPY"` still works for mode switching without overriding duration/intensity.

### 3. Offline session synchronization is not implemented

- **Severity:** High
- **Location:** `src/bluetooth.cpp:56`, `src/bluetooth.cpp:57`, `src/bluetooth.cpp:1234`, `src/bluetooth.cpp:1536`
- **Evidence / trigger:** Setup exposes only the single JSON service/characteristic. The documented session data/ack characteristics (`0000aa01...` and `0000aa02...`) do not exist, and `notifyNewSessionStored()` is an empty placeholder even though `session_stats.cpp` calls it after saving each session.
- **Impact:** Sessions recorded while the phone is disconnected remain in flash and cannot be pulled through the app's documented binary sync path. Reconnect does not notify or start upload, so user history can appear permanently missing.
- **Suggested fix:** Implement the data/ack service with the documented 20-byte summary/extension state machine and retry semantics, or replace it with a versioned session-sync protocol implemented on both app and firmware. Make `notifyNewSessionStored()` wake that state machine.

### 4. `FACTORY_RESET` leaves profiles, sessions, device time, counters, and BLE bonds behind

- **Severity:** High
- **Location:** `src/bluetooth.cpp:1362`, `src/bluetooth.cpp:1364`, `src/bluetooth.cpp:1365`; cross-module evidence: `src/storage.cpp:444`, `src/storage.cpp:457`, `include/session_log.h:9`, `include/session_log.h:10`, `src/session_stats.cpp:41`, `src/device_time.cpp:36`
- **Evidence / trigger:** The deferred reset calls only `storageFactoryReset()`, removes the custom pair marker, and reboots. `storageFactoryReset()` resets its settings structure but does not remove `/profiles.dat`, which `storageLoadProfiles()` prioritizes on the next boot. It also does not remove `/sessions.dat`, `/sess_ev.dat`, `/sess.st`, or `/devtime.dat`, reset in-RAM session counters, or call `Bluefruit.Periph.clearBonds()`.
- **Impact:** A command acknowledged as factory reset can boot back with the old profiles, history, clock, counters, and trusted phones. This is both a privacy/reset-semantics failure and a security problem when ownership changes.
- **Suggested fix:** Centralize a complete reset transaction that removes every persistent store and temp file, clears session/profile in-memory state, clears SoftDevice bonds, verifies failures, then reboots only after completion.

### 5. BLE write callbacks perform state transitions and flash/I2C work from a second task

- **Severity:** High
- **Location:** `src/bluetooth.cpp:1209`, `src/bluetooth.cpp:1240`; representative direct mutations at `src/bluetooth.cpp:968`, `src/bluetooth.cpp:1001`, `src/bluetooth.cpp:1036`, `src/bluetooth.cpp:1112`
- **Evidence / trigger:** In the installed Bluefruit library, `setWriteCallback()` defaults to an AdaCallback task, separate from the Arduino loop task. `onCharacteristicWrite()` immediately parses and executes commands. Those commands call `setDeviceMode()`, profile functions that write LittleFS, `setDeviceTime()` (also a flash write), and legacy `ACTION=CALIBRATE`, whose `startCalibration()` path changes modes, touches motor/sensor state, and sends notifications. Meanwhile the main task reads and writes the same non-atomic state and may use the same filesystem. Only a few unrelated pending flags are deferred.
- **Impact:** A write arriving during `bluetoothLoop()`, calibration, session finalization, or another flash operation creates data races and re-entrant subsystem calls. Outcomes include torn telemetry state, duplicate/incorrect session transitions, motor-state glitches, and filesystem corruption or resets.
- **Suggested fix:** Make the callback copy a bounded complete command into a thread-safe queue only. Parse and execute every command from `bluetoothLoop()` on the main task; use explicit synchronization for the few values that truly must cross tasks.

### 6. DFU entry ignores the documented low-battery and active-session guards

- **Severity:** High
- **Location:** `src/bluetooth.cpp:1054`, `src/bluetooth.cpp:1056`, `src/bluetooth.cpp:1259`, `src/bluetooth.cpp:1264`
- **Evidence / trigger:** Every `ENTER_DFU`/`OTA_DFU`/`DFU` command is marked `ARMED`; the next loop marks it `ENTERED` and jumps to the bootloader. There is no battery threshold, active training/therapy/calibration check, or error response, despite the app contract accepting `LOW_BATTERY` and `SESSION_ACTIVE` errors (`private/aligneye_ble_protocol_docs.md:58`).
- **Impact:** DFU can abandon an active session and can begin with insufficient energy, increasing the risk of a failed update or an apparently bricked device.
- **Suggested fix:** Before arming, refresh the battery reading, enforce a documented minimum percentage/voltage, reject while any session/calibration is active (or stop and persist safely by explicit policy), and return the specified structured error.

### 7. The physical pairing-unlock state is inert, and successful pairing is never persisted

- **Severity:** High
- **Location:** `src/bluetooth.cpp:30`, `src/bluetooth.cpp:31`, `src/bluetooth.cpp:450`, `src/bluetooth.cpp:519`, `src/bluetooth.cpp:1238`, `src/bluetooth.cpp:1493`
- **Evidence / trigger:** `pairingUnlockActive` and `blePairingKnownPaired` are assigned but never consulted to accept/reject pairing or choose whitelist advertising. `updatePairingLed()` is a stub. `onBleSecured()` updates RAM but never calls `saveBlePairMarker(true)`; the only marker writes pass `false`. The characteristic uses encrypted, no-MITM permissions, so a new central can use Just Works pairing without the advertised triple-click unlock flow.
- **Impact:** The physical unlock gesture does not protect control access. An unintended phone can bond and then issue mode, factory-reset, or DFU commands; after reboot the custom pairing-status marker is always forgotten.
- **Suggested fix:** Enforce a short physical unlock window in the security/pairing callback, use bond allowlisting/whitelist advertising when locked, persist successful bonding, and consider MITM authentication if unauthorized nearby control is in scope.

### 8. Therapy live packets use field names the app does not parse

- **Severity:** Medium
- **Location:** `src/bluetooth.cpp:163`, `src/bluetooth.cpp:165`
- **Evidence / trigger:** Firmware sends `elapsed` and `remaining`. The analyzed app accepts `t_elap` or `therapy_elapsed_sec`, and `t_rem` or `therapy_remaining_sec`; it also expects the therapy intensity field (`private/aligneye_ble_protocol_docs.md:71`). None of those recognized fields is present in `TL`.
- **Impact:** Therapy may run correctly on-device while the app's elapsed/remaining progress and intensity display stay stale or empty.
- **Suggested fix:** Emit the exact canonical app keys (or update the app and protocol version together), include intensity, and add a packet-contract test that feeds firmware payloads through the app parser.

### 9. Required battery/state fields are absent from all app-facing telemetry

- **Severity:** Medium
- **Location:** `src/bluetooth.cpp:121`, `src/bluetooth.cpp:262`, `src/bluetooth.cpp:290`; contract: `private/aligneye_ble_protocol_docs.md:68`, `private/aligneye_ble_protocol_docs.md:69`, `private/aligneye_ble_protocol_docs.md:70`
- **Evidence / trigger:** `INFO` omits `battery` and `dfu_ready`; `L` omits `sub_mode`, `is_bad_posture`, raw/calibration fields, and `battery`; `T` also omits `battery` and the broader live-state cache fields. Although battery is sampled and cached, no normal BLE payload sends it (the optional `S` packet is RTT-only).
- **Impact:** The app cannot reliably display battery or cache the documented live state, and it lacks the state needed to prepare/validate DFU. Optional-field fallbacks can mask the fault while presenting stale UI values.
- **Suggested fix:** Define one versioned telemetry schema, populate its required fields from a shared snapshot builder, and test `INFO`, `L`, and `T` against the actual app decoder.

### 10. Profile names are neither parsed nor encoded as JSON strings correctly

- **Severity:** Medium
- **Location:** `src/bluetooth.cpp:911`, `src/bluetooth.cpp:919`, `src/bluetooth.cpp:748`, `src/bluetooth.cpp:769`
- **Evidence / trigger:** `extractJsonStringField()` treats the next quote as the end of a value and ignores backslash escapes. Outgoing profile names are inserted with `%s` without JSON escaping. A valid app command produced by `jsonEncode`, such as a name containing `"`, `\\`, a newline, or another control character, is truncated/misread and later makes `P`, `T`, or calibration JSON invalid.
- **Impact:** User-entered profile names can be corrupted, persisted incorrectly, or break the shared BLE stream framing for subsequent messages.
- **Suggested fix:** Use a bounded JSON parser/serializer. At minimum, correctly decode JSON escapes on input and escape quotes, backslashes, and control characters on every output path; validate the resulting UTF-8/name length before storage.

### 11. The app cannot set the system-default profile as the default

- **Severity:** Medium
- **Location:** `src/bluetooth.cpp:1006`, `src/bluetooth.cpp:1009`; cross-module evidence: `src/orientation_profiles.cpp:256`
- **Evidence / trigger:** Profile ID `0` is explicitly valid for the synthetic system default, and `setProfileDefaultById(0)` supports it. The BLE handler first requires `getProfileById(id) != nullptr`; ID 0 is not in the custom profile array, so it returns `PROFILE_NOT_FOUND` and never calls the supporting API.
- **Impact:** A user who changes the default to a custom calibration cannot restore the system default through the documented JSON command.
- **Suggested fix:** Treat ID 0 as valid in `PROFILE_SET_DEFAULT`, then add command tests for both ID 0 and stored profile IDs.

### 12. Failure ACKs discard the error reason already computed by the parser

- **Severity:** Medium
- **Location:** `src/bluetooth.cpp:94`, `src/bluetooth.cpp:101`, `src/bluetooth.cpp:1069`
- **Evidence / trigger:** The parser assigns specific reasons such as `BAD_MODE`, `PROFILE_NOT_FOUND`, `CALIBRATING`, and `INVALID_SLOT`, then passes the reason to `sendCommandAck()`. That function never reads `error` and emits only `ok:false`.
- **Impact:** The app and field diagnostics cannot distinguish invalid input from temporary state or a missing profile, making recovery and user messaging unreliable.
- **Suggested fix:** JSON-escape and include a stable `error` code on failed ACKs; add tests for every rejection branch.

### 13. Legacy `CMD=CALIBRATE_START` can never read its `slot` or `name`

- **Severity:** Low
- **Location:** `src/bluetooth.cpp:1141`, `src/bluetooth.cpp:1144`, `src/bluetooth.cpp:1146`, `src/bluetooth.cpp:1150`
- **Evidence / trigger:** The second-pass parser uppercases each key, then compares it to lowercase literals `"slot"` and `"name"`. Both comparisons are always false, so the path always uses auto slot and the literal `Profile` regardless of supplied values.
- **Impact:** Legacy clients receive a successful calibration with an unexpected name, and invalid non-auto slot requests are silently treated as auto.
- **Suggested fix:** Compare against `SLOT` and `NAME` (or perform case-insensitive comparison), validate the slot, and cover this command form in parser tests.

## Risks / notes

### 14. Notification failures can leave a partial JSON object on the app stream

- **Severity:** Medium risk
- **Location:** `src/bluetooth.cpp:83`, `src/bluetooth.cpp:85`, `src/bluetooth.cpp:86`
- **Evidence / trigger:** `sendBlePacket()` ignores both `write()` and `notify()` results. Bluefruit fragments long values and can return false if notification credits are not replenished within its timeout, if the CCCD is disabled, or if the link drops mid-packet. There is no retry/queue and no packet boundary recovery.
- **Impact:** Under radio congestion or disconnect timing, the app can receive an unterminated JSON object and temporarily lose parsing of later packets. This requires link-level stress testing to quantify.
- **Suggested fix:** Check delivery status, serialize outgoing objects through a bounded queue, retry only complete frames with backpressure, and define reconnect/resynchronization behavior.
