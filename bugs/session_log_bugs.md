# Session Log Bug Audit

## Scope

Reviewed `src/session_log.cpp`, `include/session_log.h`, all repository call sites, the session finalizers, storage initialization, BLE notification path, and the documented session-upload lifecycle. No source code was changed.

## Summary

- Confirmed defects: 7
- Conditional risks / design notes: 2
- Highest-risk themes: the unsent queue has no consumer, full storage silently loses new sessions, and event identity is not unique.

## Confirmed defects

### 1. No code uploads, acknowledges, marks, or purges stored sessions

- **Severity:** High
- **Source:** `src/session_log.cpp:248`, `src/session_log.cpp:257`, `src/session_log.cpp:274`, `src/session_log.cpp:285`, `src/bluetooth.cpp:1536`
- **Evidence / trigger:** Repository-wide call-site search finds no caller for `session_log_get_unsent()`, `session_log_mark_sent()`, `session_log_purge_sent()`, or either event reader. The only completion hook, `notifyNewSessionStored()`, is an empty placeholder.
- **Impact:** Every completed session remains unsent forever. The log inevitably reaches 120 unsent entries, after which all later sessions are rejected. The documented app-synchronization lifecycle is not implemented.
- **Suggested fix:** Implement a resumable BLE transfer state machine that enumerates unsent sessions, sends summary and event data using a stable session ID, waits for an app acknowledgement, then marks and eventually purges acknowledged records.

### 2. A full unsent log silently drops the newest session

- **Severity:** High
- **Source:** `include/session_log.h:49`, `src/session_log.cpp:230`, `src/session_log.cpp:233`, `src/session_log.cpp:235`, `src/session_stats.cpp:238`, `src/session_stats.cpp:240`, `src/session_stats.cpp:243`
- **Evidence / trigger:** At `MAX_SESSIONS`, the append path removes only a sent victim. If all entries are unsent, it returns without storing anything. The API returns `void`, so the caller proceeds to append event detail and announce the session as stored.
- **Impact:** The newest completed session is lost with no error or retry. Its event record may become orphaned, counters still advance, and upstream code reports success.
- **Suggested fix:** Return a status/result enum, reserve or compact capacity before accepting a completion, and let the caller queue/retry rather than discard. Event writing and notification must be contingent on summary acceptance.

### 3. Timestamps are used as primary keys even though they are neither mandatory nor unique

- **Severity:** High
- **Source:** `include/session_log.h:18`, `include/session_log.h:29`, `src/session_log.cpp:96`, `src/session_log.cpp:109`, `src/session_log.cpp:122`, `src/session_log.cpp:131`, `src/session_log.cpp:314`, `src/session_log.cpp:357`
- **Evidence / trigger:** Neither summary nor event header stores the generated session ID. Lookup and purge associate records solely by `start_ts`; zero timestamps cause event writes to be skipped, while duplicate timestamps make `findEventRecord()` return only the first match. The lookup matches timestamp before the reader checks type, so a same-timestamp record of the other type can also hide the desired record.
- **Impact:** Sessions completed before time sync have no details, and timestamp collisions caused by stale restored time, reboot, or a backward time correction can return the wrong event payload, make a valid payload unreadable, or retain orphaned payloads during purge.
- **Suggested fix:** Persist a monotonic `session_id` in both summary and event records and use it for all lookup, acknowledgement, and compaction operations. Keep timestamp as optional metadata only.

### 4. Event readers trust `count` without validating it against `payloadLen`

- **Severity:** Medium
- **Source:** `src/session_log.cpp:107`, `src/session_log.cpp:108`, `src/session_log.cpp:387`, `src/session_log.cpp:389`, `src/session_log.cpp:397`, `src/session_log.cpp:418`, `src/session_log.cpp:420`, `src/session_log.cpp:427`
- **Evidence / trigger:** The scanner validates record boundaries using `payloadLen`, but posture and therapy readers decide how many bytes to read from `count`. They never require `payloadLen == count * 4` for posture or `payloadLen == count` for therapy.
- **Impact:** A damaged but otherwise discoverable header can make a reader consume bytes beyond that record, including subsequent headers/payloads, and return fabricated event data if enough bytes remain in the file.
- **Suggested fix:** Validate magic, type, supported version, count limits, and the exact type-specific payload length before seeking or reading. Reject and quarantine/compact malformed records.

### 5. Event compaction can replace the good file with an incomplete copy after an I/O failure

- **Severity:** High
- **Source:** `src/session_log.cpp:163`, `src/session_log.cpp:167`, `src/session_log.cpp:169`, `src/session_log.cpp:177`, `src/session_log.cpp:181`, `src/session_log.cpp:182`, `src/session_log.cpp:189`, `src/session_log.cpp:194`
- **Evidence / trigger:** `purgeOrphanedEvents()` ignores every destination write result. A short/failed source read only breaks the inner payload loop; after the scan it unconditionally deletes the original event file and promotes the temporary copy. The fallback copy also ignores write failures and then removes the temp file.
- **Impact:** Flash-full conditions or transient filesystem errors during purge can destroy previously valid event history and leave a truncated/corrupt replacement.
- **Suggested fix:** Track exact bytes read and written for every record, abort without touching the source on any mismatch, flush/close and validate the complete temp file, then perform one checked replacement.

### 6. Evicting a sent summary does not remove its event record

- **Severity:** Medium
- **Source:** `src/session_log.cpp:209`, `src/session_log.cpp:216`, `src/session_log.cpp:233`, `src/session_log.cpp:236`, `src/session_log.cpp:243`, `src/session_log.cpp:303`
- **Evidence / trigger:** When a full log has a sent slot, `session_log_append()` removes that summary and persists the new array, but it does not call event compaction. Orphan cleanup occurs only in `session_log_purge_sent()`.
- **Impact:** A system that eventually implements marking but relies on automatic full-log eviction will leak event records indefinitely and can exhaust LittleFS even though the bounded summary array remains healthy.
- **Suggested fix:** Queue event compaction whenever an eviction occurs, or adopt an append-only ID-based log with periodic validated compaction based on retained IDs.

### 7. “Atomic” summary replacement deletes the last good file before promoting the new one

- **Severity:** Medium
- **Source:** `src/session_log.cpp:29`, `src/session_log.cpp:40`, `src/session_log.cpp:48`, `src/session_log.cpp:49`, `src/session_log.cpp:53`
- **Evidence / trigger:** After fully writing `/sessions.tmp`, `persistSessions()` removes `/sessions.dat` and only then attempts the rename. A power failure or rename/open failure in that gap leaves no primary summary file; startup does not recover the completed temp file.
- **Impact:** A reset during append, mark-sent, or purge can lose the entire summary database, not only the record being changed.
- **Suggested fix:** Use LittleFS's replace/rename semantics without a pre-delete where supported, or use two generation-tagged files and select the newest fully validated generation at boot. Add boot-time temp recovery.

## Conditional risks and design notes

### 8. The raw summary file has no schema header, version, length, or checksum

- **Severity:** Medium (risk)
- **Source:** `include/session_log.h:18`, `src/session_log.cpp:35`, `src/session_log.cpp:74`, `src/session_log.cpp:78`
- **Evidence / trigger:** Native `StoredSession` objects, including compiler padding and native `bool` representation, are written directly. Loading validates only that `type` is posture or therapy.
- **Impact:** A compiler/layout change silently changes the on-flash ABI, and corruption that leaves byte zero as 1 or 2 can be accepted with arbitrary timestamps, lengths, counts, or sent state.
- **Suggested fix:** Define a packed, explicitly versioned wire record with fixed-width flags, record length, generation/ID, and CRC. Migrate or reject old versions deterministically.

### 9. Every metadata change synchronously rewrites the entire summary collection

- **Severity:** Medium (performance/endurance risk)
- **Source:** `src/session_log.cpp:29`, `src/session_log.cpp:35`, `src/session_log.cpp:244`, `src/session_log.cpp:281`, `src/session_log.cpp:302`
- **Evidence / trigger:** Append, mark-sent, and purge all call `persistSessions()`, which rewrites up to 120 records and flushes flash synchronously.
- **Impact:** Latency increases with history size and blocks the single firmware loop; repeated sent-state changes also amplify flash writes.
- **Suggested fix:** Use append-oriented records plus a small acknowledgement journal, then compact incrementally while idle.
