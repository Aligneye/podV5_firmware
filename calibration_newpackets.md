1. Profile list — response to {"cmd":"GET_CALIBRATION_PROFILE"} (s and per-profile a/d flags removed, top-level active/default IDs):


{
  "t": "P",
  "profiles": [
    {"id": 3, "n": "Desk",    "c": 1752710400, "q": 92},
    {"id": 5, "n": "Reading", "c": 1752796800, "q": 78}
  ],
  "count": 2,
  "max": 8,
  "active": 3,
  "default": 5
}
active/default are profile IDs; 0 means "system default" (no stored profile). The old per-profile a/d flags are gone — the app must match profile.id against the top-level active/default fields. Per-profile fields: id, n (name), c (created-at epoch seconds), q (quality 0-100).

2. Calibration success — DONE packet (slot removed, everything else unchanged):


{
  "t": "C", "state": "DONE", "result": "success", "save_state": "pending",
  "profile_id": 9, "name": "Desk", "quality": 92,
  "x": 0.0123, "y": 0.6789, "z": 0.7312,
  "total_samples": 480, "passed_samples": 462
}
3. Calibration failure — DONE packet (never had a slot, unchanged):


{"t": "C", "state": "DONE", "result": "failed", "reason": "LOW_QUALITY"}
4. CALIBRATE_START request — the app should now send just:


{"cmd": "CALIBRATE_START", "name": "Desk"}
If an old app still sends "slot":"AUTO" (or any slot value), the firmware ignores it instead of rejecting with INVALID_SLOT — old apps keep working, they just can't influence placement (they never actually could).

Everything else — the save_state persisted follow-up packet, the ID-based PROFILE_SELECT / PROFILE_SET_DEFAULT / PROFILE_RENAME commands — is untouched. On the firmware side, the slot parameter was also dropped from notifyCalibrationComplete() / sendCalibrationDone() and the index→slot computation in calibration.cpp is gone. The two build warnings are pre-existing (applyProfileCommand, isOrientationDetectionReady unused).