Overview:
The session_log.cpp file is responsible for storing, managing, and retrieving training and therapy session data.
Whenever a training session or therapy session is completed, its information is stored in the device memory (LittleFS). The module also keeps track of whether a session has been uploaded to the mobile application or not.
It acts as the local database of the device.

This module is responsible for:
Storing completed sessions
Loading saved sessions after device restart
Managing unsent sessions
Marking uploaded sessions as sent
Removing old uploaded sessions
Saving posture event history
Saving therapy pattern history
Reading stored events from memory

# important variables:
g_sessions[] ->	Stores all session records
g_count->	Total stored sessions
g_ready->	Checks whether module is initialized
kTempPath->	Temporary session file path
kEvTempPath->	Temporary event file path
MAX_SESSIONS->	Maximum sessions that can be stored

Function Explanation:
1. 📌 persistSessions()
Purpose: Saves all session records permanently into flash memory.
Working:
Creates a temporary file.
Writes all sessions.
Replaces the old file safely.
Prevents data corruption during saving.

2. 📌 loadFromDisk()
Purpose: Loads previously saved sessions from flash memory.
Working:
Opens session file.
Reads every stored session.
Loads valid sessions into RAM.
Ignores corrupted records.

3. 📌 appendToEventFile()
Purpose: Adds new event data into the event file.
Working:
Appends posture or therapy event data at the end of the existing file.

4. 📌 findEventRecord()
Purpose: Searches for a specific event record.
Working: Finds an event using its session timestamp and returns its location.

5. 📌 collectActiveTimestamps()
Purpose: Collects timestamps of all active (unsent) sessions.

6. 📌 isTimestampActive()
Purpose: Checks whether a timestamp belongs to an active session.

7. 📌 purgeOrphanedEvents()
Purpose: Removes event records whose parent sessions no longer exist.
Working: Keeps storage clean by deleting orphaned events.

8. 📌 ensureReady()
Purpose: Initializes the session log module.
Working:
Starts LittleFS.
Loads saved sessions.
Marks module as ready.

Called only once during startup.

9. 📌 findOldestSentSlot()
Purpose: Finds the oldest uploaded session.
Working:
Used when storage becomes full and an old sent session needs to be removed.

10. 📌 removeSlot()
Purpose: Deletes a session from memory.
Working: Shifts remaining sessions to keep the array continuous.

11. 📌 session_log_init()
Purpose: Initializes the session log system.

12. 📌 session_log_append()
Purpose: Stores a newly completed session.
Working:
Checks storage space.
Removes old sent session if needed.
Adds new session.
Saves to flash memory.

13. 📌 session_log_count_unsent()
Purpose: Returns the number of sessions that are not yet uploaded.

14. 📌 session_log_get_unsent()
Purpose: Returns a specific unsent session.
Working: Searches unsent sessions by index.

15. 📌 session_log_mark_sent()
Purpose: Marks a session as uploaded.
Working: Sets the sent flag to true and updates storage.

16. 📌 session_log_purge_sent()
Purpose: Deletes uploaded sessions.
Working: Keeps only unsent sessions and removes uploaded ones from memory.

17. 📌 session_log_write_training_events()
Purpose: Stores posture event details.
Working: Saves slouch and correction timestamps for a posture training session.

18. 📌 session_log_write_therapy_events()
Purpose: Stores therapy vibration pattern history.
Working:
Writes all executed therapy patterns into flash memory.

19. 📌 session_log_read_posture_events()
Purpose: Reads stored posture events.
Working:
Returns all slouch and correction events of a session.

20. 📌 session_log_read_therapy_events()
Purpose: Reads stored therapy pattern events.
Working: Returns all therapy vibration patterns executed during a session.

## Core Algorithm
Device Starts
      ▼

Load Sessions from Flash
      ▼

New Session Completed
      ▼

Store Session
      ▼

Save to LittleFS
      ▼

Wait for Mobile Upload
      ▼

Mark as Sent
      ▼

Delete Old Uploaded Sessions
      ▼

Keep Only Pending Sessions

## Summary
session_log.cpp acts as the session storage manager of the firmware. It stores completed training and therapy sessions, manages pending uploads, saves posture and therapy events, retrieves historical data, and maintains flash memory by removing old uploaded records. It ensures that no important session data is lost even after the device restarts.