# Storage module bug audit

## Scope

Audited `src/storage.cpp`, `include/storage.h`, the selected PlatformIO board/framework, and the profile, button, Bluetooth, and session call sites that consume this module. Findings below describe the code as written; no firmware source was changed.

## Summary

- Confirmed bugs: 9
- Severity: 1 critical, 4 high, 4 medium
- Most urgent: the raw NVMC settings page is inside the active LittleFS partition.

## Confirmed bugs

### STOR-01 — Raw settings erase/write overlaps LittleFS

- **Severity:** Critical
- **Locations:** `src/storage.cpp:25-28`, `src/storage.cpp:193-212`, `src/storage.cpp:250-269`; board selection in `platformio.ini:1-4`
- **Evidence/trigger:** The configured Adafruit nRF52832 framework places `InternalFS` at `0x6D000` for seven 4 KiB pages, ending at `0x74000`. `SETTINGS_PAGE_ADDR` is `0x73000`, the final page of that same filesystem. If a filesystem settings write fails during boot, `persist()` erases and rewrites this page directly through NVMC. `loadFromFlash()` also interprets bytes from that filesystem page as a raw `PersistedSettings` object.
- **Impact:** The fallback can corrupt `/profiles.dat`, `/sessions.dat`, `/devtime.dat`, `/sess_ev.dat`, filesystem metadata, or any other LittleFS content. Conversely, normal LittleFS allocation can overwrite the supposed raw settings backup.
- **Suggested fix:** Keep one persistence mechanism, preferably a versioned LittleFS record. If a raw page is required, reserve it outside the application, LittleFS, bootloader, and signature regions in the linker/partition definition and enforce the address with build-time assertions.

### STOR-02 — A fresh device boots with the 30-minute therapy sub-mode

- **Severity:** High
- **Locations:** `src/storage.cpp:63-75`, `src/storage.cpp:395-418`, `src/button.cpp:198`, `src/therapy.cpp:38-44`
- **Evidence/trigger:** The default persisted byte is `TRAIN_INSTANT`, whose numeric value is `2`. `storageLoadTherapySubMode()` returns that training-delay byte unchanged. Button setup assigns it to `therapySubModeIndex`, where value `2` means 30 minutes.
- **Impact:** Although therapy globals and documentation describe 10 minutes as the default, erased/new storage selects a 30-minute session.
- **Suggested fix:** Give training alert style and therapy duration separate persisted fields and defaults. Validate each against its own enum before exposing it.

### STOR-03 — Factory reset does not clear the authoritative profile file

- **Severity:** High
- **Locations:** `src/storage.cpp:444-455`, `src/storage.cpp:457-475`; reset caller at `src/bluetooth.cpp:1362-1367`
- **Evidence/trigger:** `storageFactoryReset()` clears only `g_settings` and persists `/sys_settings.dat`; it neither removes nor rewrites `/profiles.dat`. After the reset-triggered reboot, `storageLoadProfiles()` gives `/profiles.dat` priority and copies its old profiles back into RAM.
- **Impact:** Calibration profiles, active profile state, and overwrite state can reappear immediately after a command reported as `FACTORY_RESET`.
- **Suggested fix:** Make reset a coordinated transaction that clears both settings files and all other data promised by the factory-reset contract, or explicitly rewrite an empty `ProfileStore` before rebooting.

### STOR-04 — A CRC-failed NVMC record is copied into live state and then saved again

- **Severity:** High
- **Locations:** `src/storage.cpp:357-378`, `src/storage.cpp:382-390`
- **Evidence/trigger:** `loadFromFlash()` assigns `g_settings = *flash` before checking its CRC. On CRC mismatch it returns `false`, but does not restore defaults. `storageSetup()` treats the result as empty storage and calls `persist()` on the already-corrupted `g_settings` object.
- **Impact:** Corrupt values are promoted into the new filesystem settings file instead of being discarded, making corruption persistent across later boots.
- **Suggested fix:** Validate a local temporary object completely, then assign it to `g_settings` only after all checks pass. Explicitly reset `g_settings` to a known default object on every failed load path.

### STOR-05 — The advertised atomic write sequence has a power-loss data-loss window

- **Severity:** High
- **Locations:** `src/storage.cpp:132-170`, `src/storage.cpp:215-245`
- **Evidence/trigger:** Both writers flush and close the temporary file, then delete the live destination before renaming the temporary file. Power loss or a rename failure between `remove(destination)` and `rename(temp, destination)` leaves no valid live record.
- **Impact:** A brownout at exactly the point this pattern is intended to protect can erase settings or all profiles. The direct-write fallback also starts only after the old copy has been deleted.
- **Suggested fix:** Use a filesystem-supported replace/rename operation that preserves the old name until commit, or use two versioned slots with CRC and a final commit marker.

### STOR-06 — Filesystem settings accept incompatible or deliberately unchecksummed records

- **Severity:** Medium
- **Locations:** `src/storage.cpp:272-289`
- **Evidence/trigger:** The filesystem path checks exact byte count and magic, but never checks `tempSettings.version`, training-delay range, profile count, IDs, or profile validity. It also treats `crc == 0` as valid regardless of content.
- **Impact:** A same-sized record from an incompatible layout, or a damaged record whose CRC word is zero, is installed directly into live state. Bad enum and profile metadata then propagate into other modules.
- **Suggested fix:** Require the expected version and CRC for the current layout. Handle legacy layouts through explicit version-specific structures and validate every bounded field before assignment.

### STOR-07 — Version 4 raw settings have no migration path

- **Severity:** Medium
- **Locations:** `src/storage.cpp:30`, `src/storage.cpp:297-355`
- **Evidence/trigger:** The current format is version 5. The migration branch accepts only versions 1, 2, and 3, and the normal branch accepts only version 5. Version 4 falls straight through to failure even though the surrounding migration comment still describes migration to v4 and version 4 existed in this repository's prior layout.
- **Impact:** Devices upgrading from the v4 raw layout lose their stored training/calibration settings instead of migrating them.
- **Suggested fix:** Define `PersistedSettingsV4`, validate it, and explicitly translate it into v5 before saving.

### STOR-08 — A failed save can be reported as successful on retry without writing anything

- **Severity:** Medium
- **Locations:** `src/storage.cpp:395-402`, `src/storage.cpp:431-441`, `src/storage.cpp:513-524`, `src/storage.cpp:531-539`, `src/storage.cpp:546-560`
- **Evidence/trigger:** Setters modify `g_settings` before calling `persist()`/`writeProfileStore()`. If the write fails, RAM retains the new value. A caller retrying the same value hits the equality early-return and receives `true`, although persistent storage was never updated.
- **Impact:** The UI can acknowledge a durable setting that silently reverts on reboot.
- **Suggested fix:** Write a candidate copy and commit it to `g_settings` only on success, or track a dirty flag and never take the equality fast path while dirty.

### STOR-09 — Profile records have no version or integrity check

- **Severity:** Medium
- **Locations:** `src/storage.cpp:81-94`, `src/storage.cpp:173-185`
- **Evidence/trigger:** `ProfileStore` contains only a magic number and payload. A full-size file with an intact magic is accepted; there is no layout version or CRC over profile names, vectors, IDs, active index, or overwrite index.
- **Impact:** Bit corruption can silently alter calibration vectors and posture behavior while still passing the loader.
- **Suggested fix:** Add a format version, payload length, and CRC; reject invalid vectors, IDs, string termination, and indices before copying the record into live settings.

## Additional risk

`/sys_settings.dat`, `/profiles.dat`, and the raw fallback act as overlapping partial copies rather than synchronized replicas. Successful profile/active-index writes commonly return before updating the settings copy (`src/storage.cpp:489-506`, `src/storage.cpp:513-524`), so fallback behavior can restore stale state even without physical corruption.
