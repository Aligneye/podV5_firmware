Overview:
The orientation_profile.cpp file manages all orientation (calibration) profiles of the device.
Whenever a user performs a successful calibration, the generated posture reference values are saved as a profile. The module can create, update, delete, select and automatically detect these profiles during device operation.
It helps the device recognize different body orientations and apply the correct posture reference for accurate posture tracking.

This module is responsible for:
Creating new calibration profiles
Saving profile reference values
Loading saved profiles from storage
Selecting active profile
Deleting and clearing profiles
Automatically detecting the current orientation
Updating profile reference values
Managing default orientation profile

Overall Working Flow
Calibration Successful
        ▼
addCalibrationProfile()
        ▼
Save Profile
        ▼
Store in Flash Memory
        ▼
Set Active Profile
        ▼
During Device Usage
        ▼
detectCurrentOrientationProfile()
        ▼
Profile Matched ?
      │        │
     Yes       No      
      ▼        ▼
Apply      Default/Unknown
Reference    Orientation

Variable	Purpose
s_profiles[] =	Stores all orientation profiles
s_profileCount =	Total number of saved profiles
s_activeProfileIndex =	Currently selected profile
s_nextOverwriteIndex =	Next profile to overwrite when storage is full
MAX_PROFILE_COUNT =	Maximum allowed profiles (8)
PROFILE_MATCH_THRESHOLD =	Minimum similarity required for profile match
PROFILE_AMBIGUITY_MARGIN =	Prevents incorrect matching between similar profiles

# Function Explanation:
1. 📌copyProfileName()
Purpose: Copies profile name safely into profile structure.
Working:
Copies profile name.
Prevents buffer overflow.
Ensures string ends correctly.

2. 📌 magnitude3D()
Purpose: Calculates magnitude of a 3D vector.
Working: Uses X, Y and Z values to calculate total vector length.

3. 📌 normalize3D()
Purpose: Converts raw sensor values into a normalized unit vector.
Working:
Calculates vector magnitude.
Divides X, Y and Z by magnitude.
Returns normalized values.

Normalization makes profile comparison independent of sensor magnitude.

4. 📌 profileNameExists()
Purpose: Checks whether a profile name already exists.
Working: Searches all stored profiles and returns true if name is found.

5. 📌 findNextProfileName()
Purpose: Automatically generates the next available profile name.
Working:
Creates names like:
Profile 1
Profile 2
Profile 3

If a name already exists, it searches for the next free name.

6. 📌 setOrientationLabel()
Purpose: Updates the current orientation label shown by the system.
Working:
Stores the selected profile name into orientationText.

7. 📌 initProfiles()
Purpose: Initializes the complete profile database.
Working:
Clears previous data
Loads saved profiles from storage
Restores active profile
Sets posture origin
Restores orientation label

This function runs during device startup.

8. 📌 addCalibrationProfile()
Purpose: Creates a new calibration profile.
Working:
Gets latest calibration values
Saves profile name
Stores reference values
Makes profile active
Saves profile into flash memory

If storage is full, oldest profile can be overwritten.

9. 📌 addNextCalibrationProfile()
Purpose: Automatically creates the next profile after calibration.
Working: Generates next profile name and calls addCalibrationProfile().

10. 📌 deleteCalibrationProfile()
Purpose: Deletes a selected profile.
Working:
Removes profile
Shifts remaining profiles
Updates active profile
Saves updated database

11. 📌 clearCalibrationProfiles()
Purpose: Deletes all stored profiles.
Working:
Clears profile database
Resets active profile
Restores default posture reference
Saves empty storage.

12. 📌 getProfileCount()
Purpose: Returns total number of saved profiles.

13. 📌 getProfile()
Purpose: Returns profile information for a given index.

14. 📌 getActiveProfileIndex()
Purpose: Returns currently active profile index.

15. 📌 getActiveProfile()
Purpose: Returns currently active profile object.

16. 📌 selectCalibrationProfile()
Purpose: Selects a saved profile.
Working:
Updates active profile index
Sets posture reference
Updates orientation label
Saves selection into storage

17. 📌 selectDefaultCalibrationProfile()
Purpose: Restores default posture profile.
Working: Removes custom profile selection and loads default posture reference.

18. 📌 detectCurrentOrientationProfile()
Purpose: Automatically detects which saved profile matches the current device orientation.
Working:
Reads current sensor values
Normalizes sensor data
Compares with every saved profile
Calculates cosine similarity
Selects best matching profile
Rejects ambiguous matches

## This is the core function of this module.

19. 📌 updateActiveProfileReference()
Purpose: Updates reference values of currently active profile.
Working: Changes reference X, Y and Z values and saves them to storage.

20. 📌 addOrUpdateProfile0()
Purpose: Creates or updates the default vertical profile.
Working:
Creates default profile if none exists.
Otherwise updates existing reference values.


# summary
orientation_profile.cpp is the profile management module of the project. It stores calibration profiles, restores them after restart, allows profile selection and automatically identifies the current device orientation by comparing live sensor data with saved reference profiles. This module plays a key role in improving posture detection accuracy and enabling multi-orientation support.