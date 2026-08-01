# Sleep module

The sleep module puts the device into Nordic System OFF after the configured
continuous timeout in the only sleep-eligible state:

- device mode is `MODE_IDLE`
- BLE is disconnected
- calibration is not active
- no physical button gesture is being processed

The timeout is controlled by `kIdleDisconnectedSleepTimeoutMs` in `sleep.cpp`.
It is currently `5000 ms` for testing.

## Timer behavior

The timer has two runtime states:

| Device state | Timer behavior |
| --- | --- |
| Idle, BLE disconnected, no calibration or button interaction | Counts from zero |
| Any non-Idle mode | Stopped and held at zero |
| Idle with BLE connected | Stopped and held at zero |
| Calibration active | Stopped and held at zero |
| Button gesture pending | Stopped and held at zero |

The timer is restarted, not paused. If eligibility is lost while it is counting,
all elapsed time is discarded. The next eligible period starts again from zero.

Motor output, therapy activity, training alerts, haptics, LEDs, and BLE traffic do
not reset the timer. Therapy and training block the timer because their device
mode is not Idle. A BLE connection blocks it regardless of traffic.

Advertising is not a BLE connection. An Idle device that is advertising but has
no active connection remains eligible for sleep.

## Button and calibration guards

Calibration runs while the device mode is Idle, so it is checked explicitly to
prevent shutdown during calibration.

The button guard uses the OneButton state machine. It remains active while a
press, click sequence, or hold is awaiting recognition. This prevents shutdown
at the timeout boundary before the gesture can change the device mode.

## Runtime flow

`sleepTimerSetup()` initializes the timer in the stopped state during boot.

`sleepTimerLoop()` runs from the main loop and:

1. evaluates the complete eligibility condition
2. stops and clears the timer when the condition is false
3. starts a fresh timer when the condition first becomes true
4. enters sleep after the condition remains true for the configured timeout

The eligibility condition is checked again immediately inside the sleep-entry
function to protect against a state change at the timeout boundary.

## System OFF sequence

When the timeout expires, the firmware:

1. latches sleep entry to prevent re-entry
2. cancels motor feedback and turns the motor off
3. stops advertising and prepares BLE for shutdown
4. puts the posture sensor into low power
5. blinks red, blue, and green
6. plays the sleep motor pulse
7. releases the LED and motor pins
8. calls the Adafruit core's `systemOff(PIN_BUTTON, LOW)` helper

The framework helper configures the button with a pull-up and low-level GPIO
SENSE, maps the Arduino pin to the physical GPIO, and enters System OFF through
the SoftDevice when it is enabled or the POWER register when it is not.

A successful System OFF call never returns. If a debugger is attached, the
nRF52832 emulates System OFF and keeps the CPU powered; the firmware therefore
waits indefinitely after the call instead of continuing or forcing a reset.

Pressing the button wakes the nRF52 from System OFF. Wake is a full firmware boot,
not a continuation of the previous loop.

## Dependencies

The sleep module reads:

- `currentMode` from the button/mode subsystem
- `bluetoothIsConnected()` from Bluetooth
- `isCalibrating()` from calibration
- `buttonInteractionPending()` from the OneButton wrapper

It uses the motor, Bluetooth, posture-sensor, and Nordic GPIO/power APIs only for
the final shutdown sequence.
