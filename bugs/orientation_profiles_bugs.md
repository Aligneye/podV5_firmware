# Orientation Profiles Module Bug Audit

## Scope

Reviewed `src/orientation_profiles.cpp`, the profile model/API in `include/calibration.h`, calibration creation call sites, training reference use, BLE profile commands/list serialization, storage profile/index metadata, device-time APIs, and profile documentation.

## Summary

- Confirmed bugs: **9** (**2 High**, **6 Medium**, **1 Low**)
- Risks/notes: **3**
- Build status: the unmodified firmware builds successfully for `nrf52832_custom`.

## Confirmed bugs

### 1. [High] Automatic orientation-profile detection is entirely unimplemented

- **Source:** `src/orientation_profiles.cpp:270-272`, `src/training.cpp:608-647`, `src/training.cpp:649-687`, `include/calibration.h:46`
- **Evidence / trigger:** The public `detectCurrentOrientationProfile()` always returns `false`. Training contains an unused stability helper and `s_bootProfileDetectionDone`, but `trainingLoop()` never calls either the helper or detection. The profile documentation describes normalized matching, ambiguity rejection, and automatic selection as the core behavior, none of which exists in executable code.
- **Impact:** Changing device/body orientation never selects the matching stored reference. Unless the app/user manually selects a profile, posture angles can be computed against the wrong calibration.
- **Suggested fix:** Implement normalized-vector matching with explicit similarity and ambiguity thresholds, call it only after stable fresh samples, and test no-match/ambiguous/match cases. If automatic selection is no longer a requirement, remove the public API/scaffold and correct the documentation.

### 2. [High] Deleting a profile before the active profile corrupts the persisted active selection

- **Source:** `src/orientation_profiles.cpp:181-206`, `src/storage.cpp:96-103`, `src/storage.cpp:489-506`, `src/orientation_profiles.cpp:108-119`
- **Evidence / trigger:** Deletion shifts later profiles left and saves them, but updates the stored active index only when the deleted ID itself was active. Example: profiles `[A,B,C,D]`, default `D`, active `C` at index 2; deleting allowed profile `A` produces `[B,C,D]` while stored index 2 remains valid and now identifies `D`. On reboot, `initProfiles()` activates `D`. If the old index falls past the new count, storage decodes it as system default instead.
- **Impact:** The runtime selection silently changes after reboot, so posture uses a different reference than the user selected.
- **Suggested fix:** Persist the new index of `s_activeProfileId` after every deletion/compaction (or persist active profile ID rather than array index). Roll back the whole operation if either profile-list or active-selection persistence fails.

### 3. [Medium] Successful calibration metadata is discarded when the profile is saved

- **Source:** `src/calibration.cpp:136-139`, `src/calibration.cpp:224-232`, `src/orientation_profiles.cpp:140-149`, `src/bluetooth.cpp:727-756`
- **Evidence / trigger:** Calibration computes and sends a real quality and accepted-sample count, but `addCalibrationProfile()` accepts only a name and hard-codes `sampleCount = 0` and `stabilityScore = 0.0`. The profile-list packet later derives `q` directly from `stabilityScore`, so every newly calibrated profile is reported with quality zero.
- **Impact:** Profile history contradicts the just-reported calibration result, and stored sample/quality fields are unusable for UI, diagnostics, or profile selection policy.
- **Suggested fix:** Pass calibration metadata into profile creation (or cache one complete pending result) and persist the actual accepted count and quality/stability value.

### 4. [Medium] `createdAtEpoch` stores uptime rather than an epoch timestamp

- **Source:** `include/calibration.h:13`, `src/orientation_profiles.cpp:146`, `src/orientation_profiles.cpp:293`, `src/bluetooth.cpp:746-756`, `include/device_time.h:14-23`
- **Evidence / trigger:** Both creation paths assign `millis()/1000` to a field named `createdAtEpoch`, and BLE exposes it as the profile's created-at epoch. A profile created ten minutes after boot reports `600`, not a Unix timestamp; the value also resets on every reboot.
- **Impact:** The app receives invalid creation dates and cannot sort or display profile history reliably.
- **Suggested fix:** Use `getDeviceTime()` when synchronized and store `0`/an explicit unknown marker otherwise. If uptime is desired, rename the field and BLE contract.

### 5. [Medium] A corrected stale next-profile ID is not persisted unless some profile ID also needed repair

- **Source:** `src/orientation_profiles.cpp:75-105`, `src/orientation_profiles.cpp:32-37`
- **Evidence / trigger:** `initProfiles()` detects `nextId <= maxId` and changes a local variable to `maxId + 1`, but `storageSaveNextProfileId(nextId)` is inside `if (updated)`. `updated` is set only when a profile has a zero/duplicate ID, not when only the counter is stale. After settings/profile-store divergence or a failed counter write, the next add reloads the stale counter and assigns an existing ID.
- **Impact:** Duplicate IDs break lookup identity: `getProfileById()` returns the first match, activation/default/delete commands can target the wrong record, and the newly added profile may not actually become the profile used by training.
- **Suggested fix:** Track counter repair separately and persist it whenever `nextId` changes. Before assigning an ID, also verify it is unused and advance/wrap safely.

### 6. [Medium] Profile APIs report success while ignoring metadata persistence failures

- **Source:** `src/orientation_profiles.cpp:32-36`, `src/orientation_profiles.cpp:149-171`, `src/orientation_profiles.cpp:223-234`, `src/orientation_profiles.cpp:242-260`, `src/orientation_profiles.cpp:274-280`, `include/storage.h:17-25`
- **Evidence / trigger:** Storage metadata functions return `bool`, but their results are discarded. For example, add returns true even if saving the active index/default/overwrite pointer fails; select returns true even if its active-index write fails; reference update mutates RAM without rollback if the profile write fails; clear always appears successful after multiple unchecked writes.
- **Impact:** BLE can acknowledge an operation that only changed RAM. After reboot, an older active/default/reference/list state can reappear, and partial multi-write updates can leave inconsistent metadata.
- **Suggested fix:** Check every persistence result, order changes as an explicit transaction, and roll back RAM on failure. Return a status from clear/default/update APIs so BLE can send an accurate ACK.

### 7. [Medium] Unsanitized profile names can make outgoing BLE JSON invalid

- **Source:** `src/orientation_profiles.cpp:15-18`, `src/orientation_profiles.cpp:122-145`, `src/orientation_profiles.cpp:209-218`, `src/bluetooth.cpp:746-757`, `src/bluetooth.cpp:764-778`
- **Evidence / trigger:** Creation and rename copy arbitrary bytes (other than truncation) into `name`. BLE inserts that name directly between JSON quotes without escaping. A legacy name such as `Desk"A` creates malformed/injectable profile-list and calibration-completion JSON.
- **Impact:** The app can fail to parse the complete profile list/result packet; crafted names can alter the apparent JSON structure.
- **Suggested fix:** Apply a documented character/UTF-8 policy at input and JSON-escape every string at serialization. Escaping at output is still required even if input is validated.

### 8. [Medium] A configured custom default is never used as the fallback posture reference

- **Source:** `src/orientation_profiles.cpp:65-69`, `src/orientation_profiles.cpp:108-119`, `src/orientation_profiles.cpp:256-268`
- **Evidence / trigger:** `setProfileDefaultById()` stores `s_defaultProfileId`, but when no active profile is restored, `initProfiles()` calls `selectDefaultCalibrationProfile()`. That function always selects ID 0 and hard-coded `(6.75, 6.75)` rather than looking up `s_defaultProfileId`. No other runtime selection path consults the configured default ID.
- **Impact:** `PROFILE_SET_DEFAULT` changes the UI marker/deletion protection but does not provide the expected reference fallback. Clearing the active selection or losing its stored index reverts to the system constants instead of the chosen custom default.
- **Suggested fix:** Separate "system default" from "selected default profile" APIs and have fallback initialization apply `s_defaultProfileId` when it is nonzero and valid.

### 9. [Low] Auto-generated names become duplicates after a deletion

- **Source:** `src/orientation_profiles.cpp:174-178`, `src/orientation_profiles.cpp:181-200`, `src/bluetooth.cpp:870-880`
- **Evidence / trigger:** Generated names use `s_profileCount + 1`, not the next unused name. Starting with `Profile 1`, `Profile 2`, `Profile 3`, deleting `Profile 2` leaves count 2 and shifts `Profile 3`; the next add is also named `Profile 3`. Legacy name-based selection chooses the first match.
- **Impact:** The app can show indistinguishable profiles and name-based commands can select a different profile than intended.
- **Suggested fix:** Search existing names for the first unused suffix (or make names explicitly slot-based and rename shifted entries consistently). Consider rejecting duplicate user-supplied names if names are an identifier in legacy flows.

## Risks and notes

### R1. Persisted profile vectors are trusted without finite/magnitude validation

- **Source:** `src/orientation_profiles.cpp:71-100`, `src/orientation_profiles.cpp:113-116`, `src/orientation_profiles.cpp:274-280`
- **Evidence:** Loaded profiles are forcibly marked valid, and update accepts any floats. Corrupt/NaN/near-zero vectors can therefore reach training's normalization path.
- **Suggested fix:** Validate IDs, null-terminated names, finite reference components, and a plausible nonzero magnitude before marking a record valid or saving an update.

### R2. Missing default IDs are not reconciled during initialization

- **Source:** `src/orientation_profiles.cpp:69-105`, `src/orientation_profiles.cpp:256-260`
- **Evidence:** A stored nonzero default ID that is absent from the loaded/repaired profile list remains in `s_defaultProfileId`; no profile is marked default and the ID is not reset.
- **Suggested fix:** After ID repair, verify that the default exists; otherwise atomically choose system default or a documented fallback and persist it.

### N1. `addOrUpdateProfile0()` never performs its advertised update case

- **Source:** `src/orientation_profiles.cpp:284-300`, `include/calibration.h:51`
- **Evidence:** The function does work only when `s_profileCount == 0`; with an existing profile it is a no-op. There are currently no in-tree callers, so this was not counted as a runtime bug.
- **Suggested fix:** Implement the update branch with checked persistence, or rename/remove the unused API so callers cannot assume it updates profile zero.
