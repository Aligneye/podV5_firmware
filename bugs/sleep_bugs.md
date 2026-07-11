# Sleep module bug audit

## Scope

Audited `src/sleep.cpp`, `include/sleep.h`, main-loop ordering, BLE shutdown, wake-pin setup, mode transitions, and all inactivity reset call sites.

## Summary

- Confirmed bugs: 3
- Behavioral risk: 1
- Severity: 1 high, 1 medium, 1 low

## Confirmed bugs

### SLEEP-01 — A BLE mode-start command can lose a race to the expired inactivity timer

- **Severity:** High
- **Locations:** `src/main.cpp:47-56`, `src/sleep.cpp:89-94`, `src/sleep.cpp:113-136`; BLE mode changes at `src/bluetooth.cpp:527-559` and `src/bluetooth.cpp:625-641`
- **Evidence/trigger:** In each loop, Bluetooth may change IDLE to TRAINING/THERAPY, then `inactivityTimerLoop()` runs before either active mode loop. `setDeviceMode()` and the BLE start helpers do not reset inactivity. If the five-minute deadline is already reached, therapy is not running yet, training has no motor alert yet, and sleep is entered before the new mode can start. Physical button changes usually mask this with their press haptic; BLE changes do not.
- **Impact:** A valid remote start command near/after the timeout can be acknowledged but immediately power the device off.
- **Suggested fix:** Reset inactivity as part of every accepted user mode/start command, and/or count a pending non-IDLE transition as activity before evaluating sleep.

### SLEEP-02 — Failure of `sd_power_system_off()` leaves the firmware permanently half-shut down

- **Severity:** Medium
- **Locations:** `src/sleep.cpp:58-87`, especially `src/sleep.cpp:63` and `src/sleep.cpp:84-86`
- **Evidence/trigger:** The SoftDevice call's return status is ignored. If it returns an error, execution continues with `s_sleeping == true`, advertising stopped, the posture sensor powered down, and motor/LED pins released. The inactivity loop refuses to retry because of `!s_sleeping`.
- **Impact:** The device remains awake but nonfunctional and cannot recover its normal peripherals without a reset.
- **Suggested fix:** Check the return code. On failure, restore peripherals/BLE and clear `s_sleeping`, or deliberately reset into a known state.

### SLEEP-03 — Calibration holdoff stops being set after roughly 24.8 days of uptime

- **Severity:** Low
- **Locations:** `src/sleep.cpp:105-111`
- **Evidence/trigger:** With no existing holdoff, the code tests `(int32_t)(until - 0) > 0`. Once the high bit of `millis()` is set, a normal future deadline casts negative, so `s_holdoffUntilMs` remains zero until uptime wraps.
- **Impact:** Calibration completion loses its intended post-calibration sleep grace period on long-running devices.
- **Suggested fix:** Track holdoff activity separately and use unsigned elapsed-time comparisons rather than ordering absolute wrapping timestamps against a zero sentinel.

## Behavioral risk / validation required

An active BLE connection and received BLE traffic are not activity sources (`src/sleep.cpp:89-94`). Consequently, an IDLE device can disconnect and enter System OFF after five minutes even while the phone is connected and exchanging commands/telemetry. If "inactivity" is meant to include app use, reset the timer on connection/RX or include `bluetoothIsConnected()` under the desired policy.
