## Purpose
device_time.cpp is responsible for maintaining the device's internal clock. It provides timestamp management, synchronization, uptime calculation, time formatting, timezone handling, and persistent storage of time values. Even after a reboot, the device can restore the last known valid time from flash storage, making session logs and event timestamps reliable.

# Working Flow
          Device Boot
               ▼
     Initialize RTC & Variables
               ▼
     Load Saved Time From Flash
               ▼
      Is Saved Time Valid?
         ┌─────────────┐
         │             │
        Yes            No            │
         ▼             ▼
 Restore Previous   Time Status
     Base Time       = UNKNOWN
         ▼
    Device Running
         ▼
 Calculate Current Time
 Using RTC Tick Counter
         ▼
 Every 5 Minutes ⏱
         ▼
  Save Updated Time
     Into Flash
         ▼
 📤 Provide Accurate
 Time To Other Modules

## Function Descriptions
1. initDeviceTime()

Initializes the complete device time subsystem during system startup. It configures the RTC hardware, resets all internal variables, restores previously saved time from flash memory, and prepares the firmware for accurate time tracking. This function is executed only once during boot.

2. maintainDeviceTime()

Runs periodically inside the main loop to maintain the internal clock. It checks whether enough time has elapsed since the previous save operation and automatically stores the latest timestamp into flash memory. This mechanism helps preserve accurate time even after an unexpected restart.

3. setDeviceTime(uint32_t epochSeconds)

Updates the internal device clock using a valid Unix epoch timestamp received from an external source such as Bluetooth or the mobile application. It refreshes the synchronization reference and immediately saves the value into persistent storage for future recovery.

4. getDeviceTime()

Returns the current Unix timestamp maintained by the firmware. The function calculates the current time by adding elapsed RTC ticks to the stored base timestamp. If synchronization has never occurred, it safely returns zero.

5. getDeviceTicks()

Returns the raw RTC tick count currently maintained by the hardware timer. These low-level ticks are used internally for uptime calculation and conversion between elapsed time and Unix timestamps.

6. ticksToEpoch(uint64_t ticks)

Converts a stored RTC tick value into its corresponding Unix timestamp. This allows previously recorded events or session logs to be mapped back to their actual calendar date and time.

7. isDeviceTimeSynced()

Checks whether the device currently has a valid synchronized clock. It returns true after successful synchronization and false if the clock has never received a valid timestamp.

8. getDeviceTimeStatus()

Returns the current synchronization status of the internal clock. The status helps distinguish whether the time is fresh, restored from storage, or still unknown.

9. getDeviceUptimeSeconds()

Calculates how long the device has been continuously running since power-up. It uses RTC ticks instead of wall-clock time, ensuring uptime remains accurate regardless of synchronization.

10. getSecondsSinceSync()

Returns the number of seconds elapsed since the last successful clock synchronization. This information helps determine whether the stored time may require refreshing.

11. persistDeviceTime()

Immediately saves the current device timestamp into flash memory. Unlike automatic background saving, this function forces persistence and is useful after synchronization or before shutdown.

12. formatEpochUTC()

Converts a Unix timestamp into a human-readable UTC date and time string. The output follows the format YYYY-MM-DD HH:MM, making debugging and logging much easier.

13. formatEpochISO()

Formats a Unix timestamp into the internationally accepted ISO-8601 representation (YYYY-MM-DDTHH:MM:SSZ). This format is commonly used in APIs, cloud services, and JSON data exchange.

14. setDeviceTZOffset()

Stores the timezone offset used for local time conversion. By applying this offset, the firmware can display timestamps according to the user's regional timezone instead of UTC.

15. getDeviceTZOffset()

Returns the currently configured timezone offset. Other modules can use this value whenever local time formatting is required.

16. formatEpochLocal()

Converts a Unix timestamp into local time by applying the configured timezone offset. The output includes both the formatted date/time and timezone information, making logs easier for end users to understand.

## Internal Helper Functions
🔧 Function	                📌 Purpose
writePersistedAtomic() =>	Safely writes time data to flash using a temporary file to avoid corruption during power loss.
loadPersistedTime() =>	Restores the previously saved timestamp during system startup.
savePersistedTime() =>	Saves the current timestamp while minimizing unnecessary flash write cycles.
RTC2_IRQHandler() =>	Handles RTC overflow interrupts to extend timer range.
readLfclkSource() =>	Detects which low-frequency clock source is currently active.
startLfclkIfNeeded() =>	Starts the RTC low-frequency clock if it is not already running.
readTicksRaw() =>	Safely reads the RTC counter together with overflow information for accurate long-term timing.

## Conclusion

device_time.cpp acts as the central time management module of the firmware. It initializes and maintains the device clock, restores saved timestamps after reboot, periodically saves updated values to flash memory, supports timezone conversion, and provides multiple formatting utilities. Because posture sessions, therapy records, and event logs all depend on accurate timestamps, this module ensures reliable and consistent timekeeping across the entire project.