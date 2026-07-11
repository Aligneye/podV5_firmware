# Training Module Bug Audit

## Scope

Reviewed `src/training.cpp`, `include/training.h`, the training lifecycle in `src/button.cpp` and `src/main.cpp`, calibration integration, motor arbitration, BLE posture telemetry, session statistics, storage/profile integration, and the relevant documentation.

## Summary

- Confirmed bugs: **5** (**1 Critical**, **3 High**, **1 Medium**)
- Risks: **2 Low**
- Implementation note: **1**
- Fixed: **1** (step_count references removed)

## Confirmed bugs

### 1. [Critical] Returning to training can skip `trainingStart()` and leave session tracking stopped

- **Source:** `src/training.cpp:208`, `src/training.cpp:667-670`, `src/training.cpp:676-679`, `src/button.cpp:55-64`, `src/main.cpp:53-55`, `src/calibration.cpp:239`
- **Evidence / trigger:** `trainingLoop()` records entry using its file-local `s_lastModeForSession`. Leaving training calls `trainingStop()` directly from `setDeviceMode()`, but it cannot update that file-local marker. When the new mode is `MODE_IDLE`, `main.cpp` returns before `trainingLoop()`, so the marker remains `MODE_TRAINING`. A subsequent IDLE -> TRAINING transition therefore fails the condition at `training.cpp:667` and skips both `wakePostureSensor()` and `trainingStart()`. A common instance is calibration started while training: calibration forces IDLE, then success returns to TRAINING.
- **Impact:** Posture calculations resume, but `onTrainingStarted()` is not called. The new training period is not tracked or promoted/saved as a session, displayed session data can remain inactive/stale, and the one-second motor grace period is not restarted. A later non-training loop can also call `trainingStop()` a second time.
- **Suggested fix:** Give lifecycle ownership to one place. For example, update the mode marker whenever `trainingStart()`/`trainingStop()` is called, or remove direct lifecycle calls from `setDeviceMode()` and ensure the transition loop runs in IDLE. Add a regression test for TRAINING -> IDLE -> TRAINING and TRAINING -> CALIBRATION -> TRAINING.

### 2. [High] Sensor failure can leave stale bad-posture state driving the motor indefinitely

- **Source:** `src/training.cpp:149-161`, `src/training.cpp:397-403`, `src/training.cpp:448-460`, `src/training.cpp:536-563`, `src/training.cpp:672-674`
- **Evidence / trigger:** An all-zero read marks `sensorInitialized = false` and makes `updatePostureAngle()` return `false`. `trainingLoop()` ignores that result and still calls `applyTrainingMotorFeedback()`. Neither the failure path nor the failed update clears `s_forwardMotorBad`/`isBadPosture`; the motor routine therefore acts on the last valid state. If the last state was bad, the delay continues to elapse and the 500 ms on/off vibration continues without fresh sensor data.
- **Impact:** The device can continue or begin a posture alert after the accelerometer has failed, while BLE/session state also remains stale. The alert repeatedly resets the inactivity timer, so the device may remain awake as well.
- **Suggested fix:** Treat sample validity as an input to feedback. On a failed sample, clear the posture-alert timer/state, set the motor base duty to zero, mark telemetry invalid, and skip motor feedback until a fresh valid sample is available.

### 3. [High] The advertised five-second sensor retry is unreachable during an ongoing failed session

- **Source:** `src/training.cpp:126-135`, `src/training.cpp:397-402`, `src/training.cpp:572-575`, `src/training.cpp:681-683`
- **Evidence / trigger:** The retry logic is inside `trainingIngestAccelSample()`'s `!sensorInitialized` branch. Normal posture sampling returns before calling it when the flag is false (`training.cpp:397-399`), and the non-training path calls it only when the flag is true (`training.cpp:681-683`). Consequently, after a disconnect sets the flag false, an ongoing training session never reaches the retry branch; only a later mode/wake transition can call `initPostureSensor()` separately.
- **Impact:** A transient I2C/sensor fault permanently disables posture and step updates for the current session, despite the log message and code claiming periodic recovery.
- **Suggested fix:** Call the ingestion/recovery routine without the outer `sensorInitialized` guards, or move retry scheduling into a routine that is always serviced. Reinitialize alert/filter state only after a verified successful read.

### 4. [High] The posture-angle formula collapses to zero for references aligned with the global Z axis

- **Source:** `src/training.cpp:273-322`, especially `src/training.cpp:311-319`
- **Evidence / trigger:** The signed component is `pz = az - dot * vz`. For a valid calibrated reference `R=(0,0,1)`, `vz=1`, `dot=az`, and therefore `pz` is always zero. The `planeMagSq <= 0.001` fallback leaves it zero. For example, a current vector tilted 30 degrees from Z gives `dot ~= 0.866` but returns `atan2(0, 0.866) = 0 degrees`.
- **Impact:** A profile whose gravity reference is on or near Z reports good/straight posture for substantial tilt, so bad-posture detection and haptics do not work in that supported 3-D orientation. Near the singularity, sensitivity is also poorly conditioned.
- **Suggested fix:** Compute a signed angle around an explicit device sagittal/lateral axis (for example with a cross product and `atan2`), and define a stable alternate axis near singular orientations. Validate the formula with references along each principal axis.

### 5. [Medium] Negative-angle posture has contradictory UI, session, and motor classifications

- **Source:** `src/training.cpp:429-432`, `src/training.cpp:447-460`, `src/bluetooth.cpp:258-260`, `src/bluetooth.cpp:287-305`, `src/session_stats.cpp:429-448`
- **Evidence / trigger:** At `currentAngle = -35` with a 30-degree threshold, `postureText` and both BLE packet paths say `BAD POSTURE` because they test both signs. The canonical `isBadPosture` state tests only `currentAngle > kBadPostureDeg`, so it remains false; `s_forwardMotorBad` remains false and session statistics record no slouch.
- **Impact:** The app/RTT display can report bad posture while the motor remains silent and saved bad-posture counts/events say the posture was good. This makes live feedback and recorded results disagree.
- **Suggested fix:** Define one canonical predicate and use it everywhere. If backward bend is intentionally display-only, expose separate names such as `isForwardAlertPosture` and `isOutsidePostureRange` so session and BLE semantics are explicit.

## Risks and notes

### R1. [Low risk] A real free-fall sample is classified as a disconnected sensor

- **Source:** `src/training.cpp:149-161`
- **Evidence / trigger:** Exactly zero acceleration on all axes is possible during free fall, but the code treats that value as proof of connection loss and tears down sensor/filter state.
- **Impact:** A drop can turn a temporary physical condition into a latched sensor outage (and then encounters confirmed bugs 3 and 4).
- **Suggested fix:** Check the I2C transaction/register status, require repeated invalid reads, or perform a device-ID probe before declaring the sensor disconnected.

### R2. [Low risk] The calibration sampling API reports initialization, not whether it obtained a fresh sample

- **Source:** `src/training.cpp:195-198`
- **Evidence / trigger:** `trainingIngestAccelSample()` can return false because of the 10 ms rate limit, but `trainingSampleAccelForCalibration()` discards that return value and returns `sensorInitialized` instead. A caller faster than the current 50 ms calibration cadence would accept duplicated stale `rawX/rawY/rawZ` values as new samples.
- **Impact:** The current caller cadence normally masks the issue, but the public function's documented success contract is unsafe for reuse.
- **Suggested fix:** Return the ingestion result and let calibration distinguish "not due yet" from a hard sensor failure.

### N1. `loadStoredCalibration()` does not load storage

- **Source:** `src/training.cpp:222-231`, `include/storage.h:27`
- **Evidence:** The function assigns hard-coded defaults and never calls `storageLoadCalibration()`. `initProfiles()` normally applies a persisted active profile shortly afterward, so this was not counted as a confirmed modern-profile failure, but legacy/system-default calibration data is not loaded by the function its name and documentation describe.
- **Suggested fix:** Either load and validate the legacy calibration there or remove/rename the helper and document that profiles are the sole source of persisted reference data.
