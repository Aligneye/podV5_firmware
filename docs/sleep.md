Overview:
The sleep module is responsible for putting the device into true low-power system-off mode after a period of inactivity.
Its job is simple:
- watch the inactivity timer
- detect whether any active work is happening
- prepare the firmware for sleep
- arm the button as the wake source
- enter Nordic system-off sleep

This module does not store sleep reason or session state in memory.
It is intentionally minimal so it can be extended later without adding complexity.

The inactivity timer is checked in milliseconds internally, but RTT logs display it in seconds with decimals for readability.

This module is responsible for:
Monitoring inactivity across therapy, calibration, motor, and training-alert activity
Resetting the inactivity timer whenever any of those activities is detected
Triggering sleep after the timeout expires
Shutting down Bluetooth before sleep
Turning off motor output before sleep
Configuring the button as the wake-up source
Entering true deep sleep using Nordic system-off

# important variables

Variable | Purpose
--- | ---
s_lastResetMs | Last time the inactivity timer was reset
s_prevActive | Tracks whether activity was already active in the previous loop
s_sleeping | Prevents re-entering sleep while shutdown is already in progress
kInactivitySleepTimeoutMs | Inactivity timeout before sleep is triggered, currently 5000 ms

# Activity sources watched by the module

The sleep timer is reset whenever any of these are active:
- `isTherapyRunning`
- `isCalibrationRunning`
- `motorActive`
- `isTrainingMotorAlertActive`

That means even a brief active pulse should prevent sleep from triggering, as long as the loop observes it.

# Function Explanation

1. `inactivityTimerHasActivity()`
Purpose: Checks whether any activity is currently happening.
Working:
- Returns true if therapy is running
- Returns true if calibration is running
- Returns true if the motor is active
- Returns true if the training motor alert is active

This is the central condition used by the sleep watchdog.

2. `inactivityTimerSetup()`
Purpose: Initializes the inactivity timer module.
Working:
- Sets the last reset time to the current uptime
- Clears the previous-active tracking flag

Called once during startup.

3. `inactivityTimerReset()`
Purpose: Resets the inactivity timer manually.
Working:
- Stores `millis()` into `s_lastResetMs`

This is called from other modules whenever they start or produce activity.

4. `inactivityTimerLoop()`
Purpose: Main watchdog loop for sleep.
Working:
- Reads the current time
- Checks whether any activity is active
- If active, it refreshes the timer continuously
- If activity just transitioned from inactive to active, it logs `INACTIVITY: timer_reset`
- If inactive long enough, it calls `enterSleepMode()`

This is the main control function of the sleep system.

5. `inactivityTimerGetElapsedMs()`
Purpose: Returns how long the device has been inactive.
Working:
- Returns current uptime minus the last reset time

This is used by RTT logging for the readable inactivity timer display.

6. `inactivityTimerGetLastResetMs()`
Purpose: Returns the raw millisecond timestamp of the last inactivity reset.
Working:
- Returns `s_lastResetMs`

Mostly useful for debugging or future extensions.

7. `enterSleepMode()`
Purpose: Performs the actual shutdown sequence before deep sleep.
Working:
- Prevents re-entry with `s_sleeping`
- Logs the transition using RTT
- Forces the device into `MODE_IDLE`
- Cancels motor feedback and sets motor duty to zero
- Prepares Bluetooth for sleep
- Puts the posture sensor into low-power sleep
- Arms the button wake source using GPIO sense
- Calls `sd_power_system_off()`

This is the function that actually enters true low-power sleep.

8. `armButtonWakeSource()`
Purpose: Configures the button pin as a wake source.
Working:
- Sets the button pin to input pull-up
- Uses Nordic GPIO sense-low configuration so a button press can wake the chip from system-off

This is required so the device can wake from deep sleep using the button.

# Bluetooth sleep helper

`bluetoothPrepareForSleep()` is called before system-off.
Its job is to:
- stop advertising
- disconnect an active BLE connection if one exists
- avoid restarting advertising when the disconnect event is part of sleep shutdown

This is needed so BLE does not keep the chip active or immediately restart after disconnect.

# Sleep Flow

Normal runtime:
1. `setup()` initializes all modules
2. `sleepSetup()` stores the initial inactivity start time
3. `loop()` runs normally
4. `inactivityTimerLoop()` watches the activity booleans
5. If activity exists, the timer keeps getting refreshed

Sleep trigger:
1. No watched activity remains true
2. The timer exceeds `kInactivitySleepTimeoutMs`
3. `enterSleepMode()` is called
4. Device mode is forced to idle
5. Motor is stopped
6. BLE is shut down
7. Posture sensor is put to sleep
8. Button wake source is armed
9. `sd_power_system_off()` puts the chip into deep sleep

Wake:
1. Button press pulls the wake pin low
2. The nRF52 wakes from system-off
3. Firmware starts again from normal boot
4. Startup path runs as usual
5. Device begins in training mode through the existing boot flow

# Module Relationships

`sleep.cpp` depends on:
- `button.h` for `MODE_IDLE` and `setDeviceMode()`
- `therapy.h` for therapy running state
- `calibration.h` for calibration running state
- `bluetooth.h` for BLE shutdown before sleep
- `motor.h` for motor shutdown
- `training.h` for posture sensor sleep and training alert state
- `nrf_gpio.h` and `nrf_soc.h` for system-off and wake configuration

`sleep.cpp` is called from:
- `main.cpp`
- `motor.cpp`
- `therapy.cpp`
- `calibration.cpp`
- `training.cpp`

# RTT Output

The RTT debugger prints the inactivity timer in seconds with decimals:
- `inactivity_timer_s`

It also prints state booleans that affect sleep:
- `therapy_running`
- `calibration_running`
- `training_motor_alert`
- `motor_active`
- `motor_output_on`

This makes it easy to see why sleep did or did not trigger.

# Practical Notes

- The timeout is currently 5 seconds for testing.
- The internal timer stays in milliseconds.
- The RTT display converts to seconds only for readability.
- The module does not persist sleep state or reason.
- True low-power sleep here is system-off, not `WFE()`.
- A successful wake is effectively a reboot from the firmware point of view.

# Summary

`sleep.cpp` is the low-power inactivity watchdog for the firmware.
It observes runtime activity, waits for the inactivity timeout, prepares the device for shutdown, and then enters true system-off sleep with the button configured as the wake source.
On wake, the board boots normally and the existing startup flow brings the device back into training mode.
