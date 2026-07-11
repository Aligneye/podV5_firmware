# Therapy module bug audit

## Scope

Audited `src/therapy.cpp`, `include/therapy.h`, all pattern functions, duration/pattern scheduling, motor calls, mode transitions, BLE getters, and session-stat callbacks.

## Summary

- Confirmed bugs: 3
- Defensive API risk: 1
- Severity: 2 medium, 1 low

## Confirmed bugs

### THER-01 — Training session sub-mode is sourced from a permanently stale therapy global

- **Severity:** Medium
- **Locations:** `src/therapy.cpp:35`, `include/therapy.h:12-24`, `src/session_stats.cpp:181-193`; actual setting at `src/button.cpp:32`, `src/button.cpp:118-120`, and `src/bluetooth.cpp:634`
- **Evidence/trigger:** This module defines `currentTrainingDelay = TRAIN_INSTANT`, but no code ever updates it. Session stats reads that variable instead of the real `trainingSubModeIndex` whenever training starts.
- **Impact:** Every training-start log reports Instant even when the user selected Delayed or No Alerts. The stored session format also has no field for the actual selection, so the metadata is lost rather than merely displayed incorrectly.
- **Suggested fix:** Remove the duplicate `TrainingDelay` state and pass/record `TrainingAlertStyle` from its real owner.

### THER-02 — Pattern scheduling loses complete intervals when the loop is delayed

- **Severity:** Medium
- **Locations:** `src/therapy.cpp:327-360`, especially `src/therapy.cpp:341-345`
- **Evidence/trigger:** When `patternElapsed >= THERAPY_PATTERN_MS`, the code increments the index only once and assigns `patternStartMs = now`. A delay spanning two or more pattern intervals therefore advances only one pattern and discards all overshoot.
- **Impact:** After a long blocking operation/debug halt, the reported and executed pattern no longer matches the session timeline. The total-duration check can then end the session with planned patterns never executed.
- **Suggested fix:** Derive the pattern index and in-pattern offset from `now - therapyStartMs`, or advance in a bounded loop while preserving the remainder.

### THER-03 — Normal completion re-enters `therapyStop()`

- **Severity:** Low
- **Locations:** `src/therapy.cpp:302-313`; `src/button.cpp:55-67`
- **Evidence/trigger:** `therapyStop(true)` sets the therapy state idle and calls `setDeviceMode(MODE_TRAINING)`. Because `currentMode` is still `MODE_THERAPY`, `setDeviceMode()` calls `therapyStop(false)` again. Motor shutdown and `onTherapyEnded()` are invoked twice; the session callback happens to guard the second invocation today.
- **Impact:** Current persistence is protected by an idempotence check, but the stop path is re-entrant and any future non-idempotent cleanup/notification will run twice.
- **Suggested fix:** End the therapy session exactly once, then change the mode through a transition path that knows the previous module is already stopped.

## Defensive API risk

`therapyStart()` has no guard against an already-running therapy (`src/therapy.cpp:277-300`). Its current caller checks first, but any additional caller can reset timestamps/pattern state and call `onTherapyStarted()` again without closing the prior statistics session. Making the public function idempotent or returning a success/failure result would prevent silent session loss.
