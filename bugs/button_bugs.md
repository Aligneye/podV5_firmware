# Button module bug audit

## Scope

Audited `src/button.cpp`, `include/button.h`, OneButton callback flow, mode transitions, persistence calls, and the training/calibration/therapy consumers of button state.

## Summary

- Confirmed bugs: 4
- Contract mismatch/risk: 1
- Severity: 1 high, 3 medium

## Confirmed bugs

### BTN-01 — Leaving training for IDLE can prevent the next training session from starting

- **Severity:** High
- **Locations:** `src/button.cpp:55-90`, especially `src/button.cpp:63-64`; `src/main.cpp:53-55`; `src/training.cpp:649-679`; calibration return at `src/calibration.cpp:239`
- **Evidence/trigger:** `setDeviceMode()` directly calls `trainingStop()`, but training's private `s_lastModeForSession` is updated only inside `trainingLoop()`. The main loop does not call `trainingLoop()` in IDLE. Therefore a TRAINING -> IDLE -> TRAINING path (for example BLE training stop followed by restart, or successful calibration returning to training) leaves the marker equal to `MODE_TRAINING`. On re-entry, `trainingLoop()` skips `trainingStart()` and `onTrainingStarted()`.
- **Impact:** Posture feedback runs, but the new training session has no active statistics lifecycle, no new start timestamp, and no normal one-second motor grace period.
- **Suggested fix:** Give one module sole ownership of the training lifecycle. Either let `trainingLoop()` observe every mode, including IDLE, or expose a transition API that atomically updates the private marker and start/stop callbacks.

### BTN-02 — Training alert-style changes are never persisted or restored

- **Severity:** Medium
- **Locations:** `src/button.cpp:118-123`, `src/button.cpp:185-204`; persistence API at `src/storage.cpp:395-410`
- **Evidence/trigger:** A training double-click changes only `trainingSubModeIndex` and sends a notification. It never calls `saveTrainingDelay()`. On boot, `buttonSetup()` loads only `therapySubModeIndex`; it never restores a training selection.
- **Impact:** Instant/Delayed/No-alerts silently returns to Instant after every reboot, despite the storage module exposing and documenting a persistent training-delay field.
- **Suggested fix:** Persist and restore a dedicated `TrainingAlertStyle` value. Do not share its byte with therapy duration.

### BTN-03 — Transition delay can keep the old training vibration active after selecting No Alerts

- **Severity:** Medium
- **Locations:** `src/button.cpp:49-53`, `src/button.cpp:114-123`; early return at `src/main.cpp:53-55`; motor update order at `src/main.cpp:47-49`
- **Evidence/trigger:** `markSubModeChanged()` suppresses `trainingLoop()` for 250 ms. If an alert is already active when the user changes to No Alerts, the prior `g_dutyWanted` remains in the motor module. After the 70 ms press override expires, `motorUpdate()` can restore that old alert duty until the transition delay ends and training finally applies zero.
- **Impact:** The motor can continue/resume vibrating briefly after the user explicitly selects No Alerts.
- **Suggested fix:** Apply safety-reducing sub-mode changes immediately (cancel the training motor request before starting the UI delay), or keep the training control loop running during sub-mode notification delays.

### BTN-04 — Therapy persistence failure is ignored

- **Severity:** Medium
- **Locations:** `src/button.cpp:125-134`, especially `src/button.cpp:129`; `src/storage.cpp:417-423`
- **Evidence/trigger:** The return value from `storageSaveTherapySubMode()` is discarded. The in-memory index is advanced and state is announced even if LittleFS rejected the write.
- **Impact:** The device/app presents the new duration as saved, but it can revert after reboot. This is amplified by the storage module's failed-save/equality bug.
- **Suggested fix:** Check the result, keep/report a dirty state, and send an explicit persistence failure to the app or revert the selection.

## Contract mismatch / validation required

### BTN-R01 — DFU is bound to double-click while the repository contract says triple-click

- **Locations:** `src/button.cpp:105-149`, `src/button.cpp:151-171`; `CLAUDE.md:33`
- **Observed behavior:** In IDLE, a double-click enters OTA DFU. A triple-click instead clears/unlocks BLE pairing. The repository architecture description says triple-click enters DFU.
- **Risk:** If the written interaction contract is authoritative, an ordinary double-click can unexpectedly reboot into the bootloader and the documented recovery gesture does something different.
- **Suggested validation:** Decide which gesture is the product contract, then align code, mobile instructions, and documentation. Treat entry to DFU as a deliberately hard-to-trigger action.
