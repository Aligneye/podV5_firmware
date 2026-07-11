# Calibration Module Bug Audit

## Scope

Reviewed `src/calibration.cpp`, `include/calibration.h`, the training sensor API, mode/session transitions, motor behavior, BLE command and completion-packet call sites, orientation-profile creation, storage-facing behavior, and calibration documentation.

## Summary

- Confirmed bugs: **4** (**3 Medium**, **1 Low**)
- Risks: **4** (**2 Medium**, **2 Low**)
- Cross-module confirmed issue: **1**, counted in `training_bugs.md`
- Build status: the unmodified firmware builds successfully for `nrf52832_custom`.

## Confirmed bugs

### 1. [Medium] A blocked start stops the current activity before validation and leaves the requested profile name stale

- **Source:** `src/calibration.cpp:90-93`, `src/calibration.cpp:286-293`, `src/calibration.cpp:493-500`, `src/calibration.cpp:96-106`
- **Evidence / trigger:** `startCalibration()` calls `therapyStop(false)`, forces `MODE_IDLE`, and clears motor duty before checking `sensorInitialized`. If the sensor is unavailable, `calibrationStartBlocked()` only sends notifications. It does not restore the previous mode, clear `s_pendingProfileName`, set/clear the result state, or apply the normal inactivity holdoff. A named BLE request that is blocked can therefore terminate an active session; a later unnamed button/legacy calibration can reuse the old name.
- **Impact:** A calibration that never starts can unexpectedly stop therapy/training, and a later successful profile can be saved under a name from an earlier failed request.
- **Suggested fix:** Validate/recover the sensor before changing modes. On a blocked start, clear all request-scoped state (including the pending name) and either preserve the previous mode or explicitly document and report the mode transition.

### 2. [Medium] Calibration start checks failure before invoking the function that can reinitialize the sensor

- **Source:** `src/calibration.cpp:498-507`, `src/training.cpp:572-575`, `src/main.cpp:53-55`
- **Evidence / trigger:** `wakePostureSensor()` calls `initPostureSensor()` when `sensorInitialized` is false, but `startCalibration()` returns as blocked before reaching that call. In IDLE, `main.cpp` also does not run the training loop, so repeated calibration requests cannot recover a transient boot/I2C initialization failure.
- **Impact:** Calibration remains unavailable from IDLE until the user causes another wake/training transition or reboots, even if the sensor is now healthy.
- **Suggested fix:** Attempt `wakePostureSensor()`/initialization first, then verify availability with a real sample before committing the calibration state transition.

### 3. [Medium] The configured quality tiers and low-quality failure are mathematically unreachable

- **Source:** `src/calibration.cpp:72-88`, `src/calibration.cpp:136-166`, `src/calibration.cpp:430-466`
- **Evidence / trigger:** A calibration reaches `calibrationSuccess()` only if every axis has standard deviation `<= 1.0` and at least 70 samples survive. Thus average spread is `<= 1.0`, the sample penalty is never applied, and `quality = 100 - spread*25` is always at least 75. The `Acceptable` range (50-69), the `Fail` range, the `sampleCount < 70` penalty, and the `quality < 50` failure branch cannot execute for any successful input. Quality is also computed from the pre-rejection sample statistics rather than the accepted set.
- **Impact:** Calibration quality has far less discrimination than its API/documentation claims; devices can only report Good or Excellent after passing, and the explicit LOW_QUALITY failure path is dead code.
- **Suggested fix:** Design the stability gate and score on the same intended scale, compute post-rejection statistics for the saved result, and add boundary tests that actually produce all advertised labels (or remove unreachable labels/branches).

### 4. [Low] The BLE completion packet reports accepted samples as both total and passed samples

- **Source:** `src/calibration.cpp:224-232`, `src/bluetooth.cpp:764-778`
- **Evidence / trigger:** The success call passes `passedSamples` into both the `sampleCount` and `passedSamples` parameters. Bluetooth serializes those as distinct `total_samples` and `passed_samples` fields. Whenever outlier rejection removes samples, the packet still sends identical values for both.
- **Impact:** The app cannot display or audit how many samples were rejected and receives factually incorrect calibration diagnostics.
- **Suggested fix:** Pass `totalSamples` as `sampleCount` and `passedSamples` only as the accepted count.

## Cross-module confirmed issue

### C1. [High] Successful calibration can return to untracked training

- **Primary report:** `bugs/training_bugs.md`, confirmed bug 1.
- **Calibration trigger:** `src/calibration.cpp:493-496` forces IDLE and `src/calibration.cpp:239` returns to TRAINING. Because the training module's private last-mode marker is not updated while the main loop is suppressed in IDLE, `trainingStart()` can be skipped after success.
- **Impact:** The post-calibration training period runs without a new session lifecycle. The fix should coordinate calibration/mode transitions with the training lifecycle rather than patching calibration alone.

## Risks and notes

### R1. [Medium risk] The pending flags do not implement "cancel wins" semantics

- **Source:** `src/calibration.cpp:315-323`, `src/calibration.cpp:480-486`, `src/calibration.cpp:528-531`
- **Evidence / trigger:** If start and cancel requests are both queued before the handler runs, cancel is processed first while the FSM is IDLE and returns without clearing `pendingStart`; the next loop then starts calibration. Current user-facing cancel call sites first check `isCalibrating()`, which usually prevents this ordering, so it is classified as an API/concurrency risk rather than a demonstrated normal UI flow. Mixed direct/deferred callers can still create stale pending state.
- **Impact:** A future caller or mixed command sequence can issue a later cancel yet still start calibration.
- **Suggested fix:** Make cancel clear `pendingStart`, or replace the two booleans with one ordered pending command where the most recent command wins. Test both request orders in one loop interval.

### R2. [Medium risk] Stable but physically invalid accelerometer vectors can pass calibration

- **Source:** `src/calibration.cpp:391-395`, `src/calibration.cpp:430-473`
- **Evidence / trigger:** Validation checks variance and sample count, but never checks that the mean magnitude is plausibly near gravity or within LIS3DH range. A sensor stuck at a constant nonzero vector has near-zero standard deviation and receives an excellent score.
- **Impact:** A hardware/register fault can be saved as a high-quality posture reference and make later posture output meaningless.
- **Suggested fix:** Validate finite values, mean vector magnitude, sensor range/status, and preferably fresh-sample progression before accepting stability.

### R3. [Low risk] Legacy BLE calibration bypasses the deferred request path

- **Source:** `src/bluetooth.cpp:669-677`, `src/bluetooth.cpp:690-694`, `src/calibration.cpp:480-482`
- **Evidence / trigger:** `CALIBRATE=START` uses the deferred wrapper, while legacy `ACTION=CALIBRATE` calls `startCalibration()` directly from the characteristic-write flow. This bypasses the design that defers mode, motor, sensor, and notification work to the main loop.
- **Impact:** Behavior and execution context differ by command format, increasing reentrancy/timing risk and making pending-flag races harder to reason about.
- **Suggested fix:** Route every external start request through `requestCalibrationStart()`.

### R4. [Low risk] The previous calibration remains marked valid while a new calibration is running

- **Source:** `src/calibration.cpp:141-144`, `src/calibration.cpp:488-526`, `src/calibration.cpp:281-284`
- **Evidence / trigger:** `startCalibration()` does not clear `s_lastCalibrationValid`; it is cleared only by failure/cancel or set during initialization. Any future/concurrent caller of `addCalibrationProfile()` during a new run could duplicate the previous vector instead of rejecting the operation.
- **Impact:** Current in-tree save calls happen only on success, so this is an API-state hazard rather than a demonstrated normal-flow failure.
- **Suggested fix:** Invalidate the cached result at the beginning of every attempt and set it true only after the new averages pass all checks.
