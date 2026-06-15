## Overview
This module manages **posture training** and **therapy sessions** in the system.  
It tracks session time, posture events, stores session history in flash memory, and provides APIs for UI/Bluetooth.

## Key Features
- Training session tracking (slouch + correction events)
- Therapy session tracking (pattern-based)
- Auto session timing using millis + RTC fallback
- Persistent storage (LittleFS)
- Session analytics (duration, counts)
- Auto promote/discard logic (30s threshold)
- RTT debug logs

---

## Session Types

1. Training Session
Tracks:
- Slouch events (bad posture)
- Correction events
- Session duration
- Posture quality stats

 Saved only if duration ≥ 30 sec  
 Otherwise discarded

2. Therapy Session
Tracks:
- Predefined therapy patterns
- Pattern execution time
- Session duration + pattern stats

 Saved if duration ≥ 30 sec


3. Persistent State
Stored in flash:
- nextSessionId
- trainingSessionCount
- therapySessionCount
- last start/end timestamps

File:
Atomic write used to prevent corruption.

## Core Functions explanations:

1. initSessionStats()
Loads saved state, resets runtime buffers, initializes session system.

---

2. Training Flow

# onTrainingStarted()
- Starts session
- Resets buffers
- Stores start time + ID

# updateSessionStats()
- Detects posture change
- Logs slouch/correction events
- Promotes session after 30s

# onTrainingEnded()
- Saves if promoted
- Discards if short session

# finalizeTrainingRecord()
- Calculates duration
- Computes bad posture time
- Stores session in flash + logs events

3. Therapy Flow

# onTherapyStarted()
- Starts therapy session
- Assigns session ID

# onTherapyEnded()
- Saves or discards session

# finalizeTherapyRecord()
- Stores therapy pattern sequence
- Logs pattern-wise breakdown
- Saves session metadata

## APIs (for UI / BLE)

### Training
getTrainingSessionNumber()
getTrainingSessionDurationSec()
getTrainingSessionBadPostureCount()
isTrainingActive()
Therapy
getTherapySessionNumber()
getTherapySessionDurationSec()
isTherapyActive()
Time Helpers
getActiveTrainingStartEpoch()
getActiveTherapyStartEpoch()

 Session Promotion Logic
A session becomes valid only if:
duration ≥ 30 seconds

Otherwise:
SESSION DISCARDED
 Event Tracking
 Slouch Event

Stored when posture becomes bad:
slouchBuf[]
 Correction Event
Stored when posture becomes normal:
correctionBuf[]
Each event = time offset (seconds from session start)

  Storage Flow
Start Session
   ↓
Collect posture events
   ↓
Check 30s threshold
   ↓
Promote / Discard
   ↓
Save to flash + session log

# Maintenance
maintainSessionStats(): sync time + fix missing timestamps
resetAllSessionCounters(): factory reset all session data

Debug Output
Uses RTT logging:

Session start/end
Event tracking
Saved/discarded status
Pattern breakdown

# Dependencies
device_time.h
therapy.h
session_log.h
bluetooth.h
InternalFS (LittleFS)
RTTStream

## Summary
This module is the core session engine that:
Tracks posture & therapy sessions
Logs detailed events
Stores persistent analytics
Provides APIs for external modules

It ensures reliable session recording even after reboot.