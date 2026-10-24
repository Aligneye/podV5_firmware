# AlignEye Pod V6.3 Firmware Change Plan

**Status:** Draft for discussion  
**Hardware:** `NEW_V6_3` schematic  
**Firmware baseline:** Current nRF52832 firmware in this repository  
**Purpose:** Adapt the firmware to the V6.3 hardware without losing device behavior or allowing incompatible firmware to be released to the wrong board revision.

No firmware changes are authorized by this document. Implementation should begin only after the decisions in the Open Decisions section are agreed.

## 1. Schematic Changes Reviewed

The V6.3 schematic identifies these hardware changes:

1. The red and green LED nets were removed from MCU pins P0.13 and P0.15.
2. A pull-up/bias circuit was added to the TP4056 `TEMP` input.
3. The battery protection circuit was corrected, including the duplicated `BAT-` connection and a current-limiting resistor on the protection IC `VM` input.
4. Current-limiting resistors were added around the RGB LED.
5. The direct ground connection to `BAT-` was removed so it does not bypass the protection circuit.
6. The TP4056 temperature-network resistor values were calculated and added.
7. The tactile switch was replaced by a smaller part.
8. The RGB LED component and its connections were replaced and verified.
9. The TP4056 `RPROG` resistor was changed to 3.6 kOhm, setting charge current to approximately 333 mA.
    
## 2. V6.3 Firmware-Facing Pin Map

| Function | MCU connection | Firmware status |
|---|---:|---|
| Button | P0.11 | Unchanged |
| Blue LED | P0.14 | Firmware definition must change |
| Red LED | TP4056 `CHRG#` | No longer controlled by MCU |
| Green LED | TP4056 `STDBY#` | No longer controlled by MCU |
| Motor MOSFET | P0.17 | Unchanged |
| Battery measurement | P0.02 / AIN0 | Unchanged |
| LIS3DH SDA | P0.26 | Unchanged |
| LIS3DH SCL | P0.27 | Unchanged |
| LIS3DH address | `0x18` with SA0 low | Compatible with current firmware |
| Reset | P0.21 / RESET | Hardware reset; not application-controlled |

The current firmware defines red, green, and blue as P0.13, P0.14, and P0.15. That definition does not match V6.3: only the blue LED remains under MCU control, and it is connected to P0.14.

## 3. Required Firmware Changes

### 3.1 Board configuration

Update `include/config.h` to describe the V6.3 board accurately:

- Change the board header from the old V4 description to V6.3.
- Keep the existing button, motor, battery ADC, and I2C assignments.
- Define the firmware-controlled blue LED as P0.14.
- Remove the red and green MCU LED definitions.
- Document that the blue LED is active-low because the RGB LED is wired as a common-anode device.
- Do not reuse P0.13 or P0.15 for another feature until the PCB and production test requirements explicitly assign them.

### 3.2 LED control abstraction

Replace the RGB-specific helpers in `src/bluetooth.cpp` with a blue-status-LED abstraction:

- Provide explicit operations for off, on, PWM brightness, and timed patterns.
- Configure only the blue LED GPIO during startup.
- Ensure no firmware path configures or drives the red and green charger LED nets.
- Preserve active-low PWM behavior.
- Keep LED timing non-blocking during normal operation.

This abstraction should be owned by a dedicated LED module if the pattern logic would otherwise remain duplicated between Bluetooth and sleep handling.

### 3.3 Battery indication

The current physical battery indication uses green, yellow, and red. V6.3 cannot reproduce those colors because red and green are controlled directly by the charger.

The proposed blue-only replacement is:

| Battery level | Proposed blue indication |
|---|---|
| 67-100% | Three pulses |
| 34-66% | Two pulses |
| 0-33% | One pulse |

The exact pulse timing should remain visibly distinct from the rapid pairing-window blink and the shutdown sequence. BLE battery percentage reporting remains unchanged and continues to provide the precise value to the app.

### 3.4 Pairing indication

Replace the current rapid red pairing-window blink with a rapid blue blink:

- Start it only while the physical pairing window or owner-claim commit is active.
- Stop it immediately after successful authorization, expiry, cancellation, or failure cleanup.
- Preserve the existing pairing security and timing behavior; only the physical indication changes.

### 3.5 Sleep indication and GPIO release

Update `src/sleep.cpp`:

- Replace the red-blue-green shutdown sequence with a distinct blue-only sequence, initially three short pulses.
- Release only the blue LED GPIO before entering System OFF.
- Preserve motor shutdown, LIS3DH power-down, BLE shutdown, and button wake configuration.
- Verify that the released blue pin cannot glow weakly during System OFF.

### 3.6 Battery ADC acquisition

The V6.3 battery divider is 470 kOhm over 470 kOhm with a 100 nF capacitor on `BATPER`.

- Keep the 2:1 voltage conversion because the divider ratio has not changed.
- Treat the divider as a 235 kOhm Thevenin source for conservative SAADC configuration.
- Change the nRF52832 SAADC acquisition time from the 3 microsecond framework default to at least 20 microseconds during a battery reading.
- Restore shared ADC settings after the measurement so future analog users are not affected.
- Account for the approximately 23.5 ms divider/filter time constant at startup.
- Review the existing 16 samples and 2 ms inter-sample delays. The hardware filter may allow fewer or non-blocking samples, but the sampling strategy should only be changed after measurements demonstrate equal or better accuracy.
- Do not change the voltage-to-percentage lookup table until V6.3 boards have been characterized against a DMM.

### 3.7 Hardware and firmware identity

Update hardware identity only as part of a coordinated firmware and backend release:

- Change `HW_VERSION` from `POD_V5` to `POD_V6_3`.
- Use a V6-specific device model and OTA release channel if V5 and V6 firmware are electrically incompatible.
- Update the hard-coded device model and hardware revision in `.github/workflows/firmware-release.yml`.
- Confirm that the mobile application and firmware backend recognize the new identity before releasing the first V6.3 build.
- Prevent a V5 firmware package from being advertised to or installed on V6.3 hardware, and vice versa.

The application-facing device model should not be renamed until its compatibility and OTA consequences are agreed.

### 3.8 Documentation

Update firmware documentation that still describes a V4/V5 board or MCU-controlled RGB LED:

- Current hardware and pin map.
- LED ownership and status patterns.
- Battery measurement circuit and ADC settings.
- Sleep indication.
- Device model, hardware revision, and release process.
- Production test expectations for charger LEDs and the MCU blue LED.

## 4. Changes That Do Not Require Firmware Logic

The following schematic changes should be covered by hardware validation but do not directly require application logic changes:

- TP4056 `TEMP` resistor network.
- TP4056 3.6 kOhm `RPROG` resistor and approximately 333 mA charge current.
- Battery-protection IC connection corrections.
- Removal of the protection-bypassing `BAT-` ground connection.
- Added motor/protection current-limiting resistors.
- New tactile-switch package, provided it remains active-low to ground.
- RGB LED resistor values, except for possible blue PWM brightness tuning after bench testing.

These changes can still affect observable behavior. Charging temperature, charge time, LED brightness, voltage sag, protection cutoff, and switch feel must be verified on physical boards.

## 5. Functional Limitation

Software cannot preserve programmable red, yellow, and green indications on V6.3 because the MCU is no longer electrically connected to the red and green LED channels.

The firmware can preserve the purpose of each indication through blue blink patterns and BLE telemetry, but it cannot preserve the original colors. If programmable RGB behavior is a requirement, the hardware must route those LED channels to MCU GPIOs or provide separate charger-status and firmware-status LEDs.

The MCU also cannot report a reliable charging state because TP4056 `CHRG#` and `STDBY#` are not connected to MCU inputs. Red and green charging indications remain hardware-only.

## 6. Implementation Order

1. Confirm the LED behavior and V5 compatibility decisions.
2. Add the V6.3 board configuration and hardware identity strategy.
3. Implement blue-only LED control and remove all red/green GPIO access.
4. Migrate battery, pairing, and sleep indications to the new patterns.
5. Update battery ADC acquisition while preserving the existing public battery API.
6. Update documentation and release metadata.
7. Build the firmware and run static pin-reference checks.
8. Perform staged bench validation on a V6.3 board.
9. Run full functional regression before creating an OTA release.

## 7. Validation Plan

### 7.1 Static and build checks

- Firmware builds successfully for the nRF52832 target.
- No references to `PIN_LED_RED` or `PIN_LED_GREEN` remain.
- Every firmware GPIO matches the approved V6.3 netlist.
- Battery conversion still uses the correct 2:1 ratio.
- V6.3 version metadata is consistent between firmware and the release workflow.

### 7.2 LED and charging checks

Test with USB disconnected, charging, and charge complete:

- Blue status LED operates from P0.14 with correct polarity.
- Red charger LED indicates TP4056 charging.
- Green charger LED indicates TP4056 standby/charge completion.
- Firmware never interferes with red or green charger indication.
- Pairing, battery, and sleep blue patterns are visually distinguishable.
- Mixed charger and blue indications remain understandable through the enclosure/light pipe.

### 7.3 Battery checks

Compare firmware readings with a calibrated DMM at several battery voltages, including approximately 4.2 V, 3.9 V, 3.7 V, and the low-battery region.

Repeat measurements while:

- USB charging is active.
- BLE is transmitting.
- The motor is off.
- The motor starts and runs at maximum intended duty.

Record raw ADC, sensed millivolts, calculated battery millivolts, reported percentage, and DMM voltage. Establish acceptable production tolerances before adjusting the percentage curve.

### 7.4 Functional regression

- All single, double, triple, and hold button gestures.
- Idle, Training, Therapy, calibration, and DFU transitions.
- Motor patterns and intensity levels.
- LIS3DH initialization, sampling, and I2C recovery.
- BLE advertising, secure pairing, reconnect, telemetry, and commands.
- Session creation, persistence, and synchronization.
- System OFF entry, sleep current, and button wake.
- Factory flashing and OTA update using V6.3 identity.

### 7.5 Protection and power checks

- Motor startup does not reset the MCU or disconnect BLE.
- Battery protection disconnects the load at the intended limits.
- Protected `BAT-` is not bypassed through PCB ground or test equipment.
- Charge current and TP4056 temperature stay within the approved battery and enclosure limits.
- Battery readings remain stable across motor, BLE, and charger load changes.

## 8. Acceptance Criteria

The migration is complete when:

- All approved V6.3 pin assignments are implemented and reviewed.
- The firmware drives only the blue channel of the RGB LED.
- Battery, pairing, and sleep indications retain clear user-visible meanings.
- Existing BLE, sensor, motor, button, storage, session, calibration, therapy, training, sleep, and OTA functionality passes regression testing.
- Battery voltage accuracy meets the agreed tolerance on representative V6.3 boards.
- Charger and protection behavior passes electrical and thermal validation.
- Firmware, application, backend, and release metadata agree on V6.3 compatibility.
- No V5/V6 cross-release path can deliver incompatible firmware.

## 9. Open Decisions

1. Are blue pulse patterns acceptable as the replacement for programmable battery colors and the red pairing indication?
2. Must this repository continue producing firmware for V5 boards, or will it become V6.3-only?
3. If V5 remains supported, should board revisions use separate PlatformIO environments and compile-time pin maps?
4. Should the product model become `ALIGN_POD_V6`, or should only the hardware revision change while the product-family model remains stable?
5. What battery-voltage accuracy and percentage stability are required for production acceptance?
6. Is hardware-only charger status sufficient, or must a future board revision route `CHRG#` and `STDBY#` to MCU inputs?

