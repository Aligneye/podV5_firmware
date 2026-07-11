# Motor module bug audit

## Scope

Audited `src/motor.cpp`, `include/motor.h`, its priority layers, mode-dependent intensity scaling, rollover handling, and all current callers.

## Summary

- Confirmed bugs: 3
- Behavioral risk: 1
- Severity: 1 medium, 2 low

## Confirmed bugs

### MOTOR-01 — Normal duty writes bypass an active calm-haptic layer

- **Severity:** Medium
- **Locations:** `src/motor.cpp:59-76`, `src/motor.cpp:91-98`, `src/motor.cpp:111-119`, `src/motor.cpp:146-159`
- **Evidence/trigger:** `motorUpdate()` gives calm haptics priority over base duty, but `motorSetDuty()` checks only `overrideActive()` and then calls `applyDuty()` directly. Any therapy/training base write during a calm haptic immediately replaces the calm waveform until the next `motorUpdate()` iteration; frequent base writes repeatedly overwrite it.
- **Impact:** The public calm-haptic API cannot maintain the priority promised by the module's update logic.
- **Suggested fix:** Have `motorSetDuty()` update only `g_dutyWanted`, or check both active priority layers before applying output. Centralize all physical writes in `motorUpdate()`.

### MOTOR-02 — A one-millisecond calm haptic is accepted but can never produce output

- **Severity:** Low
- **Locations:** `src/motor.cpp:66-75`, `src/motor.cpp:111-119`
- **Evidence/trigger:** Any nonzero duration is accepted, but `durationMs == 1` makes `half == 0`; `calmHapticDuty()` then always returns zero.
- **Impact:** The API reports no failure yet silently produces no haptic for a valid input.
- **Suggested fix:** Reject/clamp durations below two milliseconds, or calculate the envelope without integer half-duration collapse.

### MOTOR-03 — Override expiry can collide with the zero sentinel at `millis()` rollover

- **Severity:** Low
- **Locations:** `src/motor.cpp:52-57`, `src/motor.cpp:100-109`
- **Evidence/trigger:** Zero means "no override," but expiry is stored as `millis() + durationMs`. Near the 32-bit wrap boundary that sum can equal zero, so the just-created override is considered inactive immediately.
- **Impact:** One feedback pulse can be skipped during the narrow rollover window (approximately every 49.7 days of uptime).
- **Suggested fix:** Store an explicit active boolean plus a start time and compare unsigned elapsed duration.

## Behavioral risk / validation required

`applyDuty()` scales every motor request whenever `currentMode == MODE_THERAPY` (`src/motor.cpp:32-38`), including button, connection, and disconnection override haptics. If `therapyIntensityLevel` is meant to affect therapy patterns only, UI/system feedback is incorrectly weakened in therapy mode. Pass the request class to the motor layer or pre-scale only therapy base requests.
