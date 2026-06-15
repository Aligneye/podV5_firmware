# Overview
The storage.cpp file is responsible for saving and loading important device data permanently.
Whenever the device stores settings like training delay, calibration values, orientation profiles, or active profile information, this module writes them into flash memory or the internal file system. When the device restarts, this file loads the saved data back into RAM so the user does not lose previous settings.

In simple words, this module works as the permanent memory manager of the firmware.

# This module is responsible for:
Saving device settings into flash memory
Loading saved settings after restart
Storing calibration values
Managing orientation profiles
Managing active profile selection
Managing profile overwrite index
Initializing session log storage
Handling backward compatibility of old storage versions

# Overall Working Flow
Device Starts
      ▼
storageSetup()
      ▼
Load Data From Flash
      ▼
Data Found ?
   │          │
  Yes         No
   ▼          ▼
Load Data   Save Default Values
      ▼
Other Modules Access Storage
      ▼
Whenever Data Changes
      ▼
Persist Updated Data Into Flash


# Important Data Structures
1. PersistedSettings:
Stores all important device settings like:
Training delay
Calibration values
Profile count
Orientation profiles
This structure is permanently stored inside flash memory.

2. ProfileStore
Used for storing orientation profiles inside the internal file system.
It keeps:
Number of profiles
Active profile
Profile list


## Function Explanation
1. 📌 storageSetup()
Purpose: Initializes the storage module when the device boots.
Working:
This function first tries to load previously saved settings from flash memory. If valid data is found, it restores those settings into RAM. If storage is empty or corrupted, it creates default values and saves them into flash. It also initializes the session logging module.

2. 📌 saveTrainingDelay()
Purpose: Saves the selected training delay permanently.
Working:
It validates the incoming delay value and checks whether it is already stored. If the value has changed, it updates the storage structure and writes the new setting into flash memory so it remains available after device restart.

3. 📌 loadTrainingDelay()
Purpose: Loads the saved training delay.
Working:
This function reads the stored training delay from memory. If the stored value is invalid, it safely returns the default training delay to avoid unexpected behavior in the application.

4. 📌 storageLoadTherapySubMode()
Purpose: Returns the saved therapy/training sub-mode.
Working:
This function simply retrieves the previously stored training delay and provides it to other modules that require the current sub-mode during execution.

5. 📌 storageSaveTherapySubMode()
Purpose: Stores the selected therapy sub-mode.
Working: It converts the selected mode into the internal format, saves it permanently, and prints a debug message through RTT so developers can verify that the setting has been updated successfully.

6. 📌 storageLoadCalibration()
Purpose: Loads saved calibration values.
Working:
The function copies the stored Y-axis and Z-axis calibration values into the provided variables. These values are later used by the posture detection algorithm for accurate angle calculation.

7. 📌 storageSaveCalibration()
Purpose: Stores calibration values permanently.
Working:
Before saving, it validates the calibration values and replaces invalid readings with safe defaults. If the values have changed, they are written into flash memory and a confirmation message is printed.

8. 📌 storageLoadProfiles()
Purpose: Loads all saved orientation profiles.
Working:
The function first tries to read profiles from the internal file system. If successful, it restores all saved profiles into memory. If no file exists, it falls back to the profiles stored inside flash memory.

9. 📌 storageSaveProfiles()
Purpose:
Stores orientation profiles permanently.
Working:
This function copies all orientation profiles into the storage structure and updates the profile count. It also keeps the default calibration values synchronized with the first profile before saving everything into permanent storage.

10. 📌 storageLoadActiveProfileIndex()
Purpose: Returns the currently active profile.
Working:
It reads the stored active profile index from memory and returns it so the firmware knows which orientation profile should be used for posture detection.

11. 📌 storageSaveActiveProfileIndex()
Purpose: Stores the active profile index.
Working:
Whenever the user changes the selected profile, this function updates the stored index and saves it permanently. This ensures the same profile remains active even after restarting the device.

12. 📌 storageLoadNextProfileOverwriteIndex()
Purpose: Loads the next overwrite position.
Working:
The function returns the profile slot that should be replaced when the maximum number of profiles has already been reached.

13. 📌 storageSaveNextProfileOverwriteIndex()
Purpose:
Stores the next overwrite position.
Working:
It saves the index of the next profile slot to overwrite, allowing the firmware to replace old profiles in a controlled circular manner instead of deleting data randomly.

## Summary

storage.cpp acts as the persistent storage manager of the firmware. It ensures that important information such as training settings, calibration values, orientation profiles, and active profile selection are safely stored and restored across device restarts. By managing flash memory and the internal file system, it provides reliable long-term storage for the entire project.