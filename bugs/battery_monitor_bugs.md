# Battery Monitor Module Bug Audit

## Scope

Reviewed `src/BatteryMonitor.cpp`, `include/BatteryMonitor.h`, its sole owner in `src/bluetooth.cpp`, the Adafruit nRF52 ADC core used by this build, and the V5 schematic battery divider (`private/schematic_page.png`).

## Summary

- Confirmed findings: **2** — 1 high, 1 medium
- Engineering risks/notes: **1** — low
- The 2:1 conversion ratio itself matches the two 220 kOhm resistors shown in the schematic.

## Confirmed findings

### 1. The ADC uses a 3 us acquisition time with a 110 kOhm source

- **Severity:** High
- **Location:** `src/BatteryMonitor.cpp:17`, `src/BatteryMonitor.cpp:18`, `src/BatteryMonitor.cpp:24`
- **Evidence / trigger:** The schematic uses a 220 kOhm / 220 kOhm divider, whose Thevenin source resistance is 110 kOhm. The Adafruit core defaults `analogSampleTime` to 3 us, and this module never changes it. Nordic's nRF52832 SAADC acquisition table rates 3 us only up to 10 kOhm, 10 us up to 100 kOhm, and 15 us up to 200 kOhm. Thus this board is outside the configured settling specification on every battery read.
- **Impact:** The sample capacitor may not settle to the divider voltage, biasing readings (typically low) and making battery percentage/color/DFU decisions inaccurate or unit-dependent.
- **Suggested fix:** Set `analogSampleTime(15)` or a conservatively validated longer value before sampling this 110 kOhm source, then restore the prior/default ADC configuration if other analog consumers are added. Validate against a DMM over battery voltage and temperature. Reference: [Nordic nRF52832 Product Specification](https://docs-be.nordicsemi.com/bundle/nRF52832-PS/raw/resource/enus/nRF52832_PS_v1.8.pdf), SAADC acquisition-time section.

### 2. Every cached battery refresh blocks the control loop for about 33 ms

- **Severity:** Medium
- **Location:** `src/BatteryMonitor.cpp:20`, `src/BatteryMonitor.cpp:22`, `src/BatteryMonitor.cpp:25`; call cadence: `src/bluetooth.cpp:401`, `src/bluetooth.cpp:406`
- **Evidence / trigger:** One read waits 1 ms, then performs 16 samples with a 2 ms delay after every sample: at least 33 ms plus conversions. `bluetoothLoop()` invokes it every five seconds, including during training, therapy, and calibration.
- **Impact:** The nominal 100 Hz posture/calibration loop misses roughly three samples every refresh, command processing is delayed, and software-updated therapy/motor patterns hold their previous value for the stall. This is deterministic, not only a worst-case timing estimate.
- **Suggested fix:** Use SAADC hardware oversampling or a nonblocking sampler spread across loop iterations, cache the completed result, and avoid `delay()` in the periodic control path.

## Engineering risks / notes

### 3. Percentage thresholds have no board calibration or hysteresis

- **Severity:** Low risk
- **Location:** `src/BatteryMonitor.cpp:34`, `src/BatteryMonitor.cpp:59`
- **Evidence / trigger:** Conversion assumes an exact 3.000 V full-scale and exact 2:1 divider, then maps voltage through hard 10% steps with no hysteresis, load compensation, charger-state handling, or per-board gain/offset calibration.
- **Impact:** Resistor/reference tolerance and motor/charger load can move readings across adjacent bands, causing percentage and LED color to jump. Hardware measurements are needed to determine magnitude after the acquisition-time bug is fixed.
- **Suggested fix:** Characterize production boards, apply calibrated slope/offset and an appropriate Li-ion discharge curve, and add filtering/hysteresis at display thresholds.
