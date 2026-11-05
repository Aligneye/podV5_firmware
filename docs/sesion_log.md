Overview:
session_log.cpp is the device's local session database. Every completed
training or therapy session is stored in LittleFS as one self-contained,
CRC-checked record keyed by its session id, and stays there until the mobile
app acknowledges it. The record format and the BLE sync protocol are
specified in docs/session_sync_v2.md.

This module is responsible for:
Storing completed sessions (one record per session)
Rebuilding the session index after a restart
Detecting and dropping damaged records (power loss mid-write)
Tracking which sessions the app has received
Removing sessions after they are synced
Keeping flash use bounded (120 records / 8 KB)
Migrating the old v1 files once

# Files
/sess_v2.log -> append-only log of session records + "sent" markers
/sess_v2.tmp -> temporary file used while compacting the log
/sessions.log, /sess_ev.dat, /sessions.dat -> legacy v1 files, migrated and deleted at boot

# Record layout
header : 'SREC' · version · type · flags · session_id · start_ts · body_len
body   : events (training: slouch start/duration, therapy: pattern/start/duration)
         + totals (training: total/slouches/bad/good, therapy: total/patterns)
footer : 'SEND' · session_id · crc32

# Important variables
g_index[]     -> RAM index of stored records (id, type, start, file offset, length, sent)
g_count       -> number of indexed records
g_fileSize    -> bytes of the log known to be on flash
g_pendingBuf  -> records/markers queued for the next flush
g_needCompact -> the file holds dead or damaged bytes and must be rewritten

# Public functions
1. session_log_init()          Start LittleFS, recover an interrupted compaction, replay the log, migrate v1 files.
2. session_log_loop()          Flush queued writes ~400 ms after they were queued (retries with backoff on failure).
3. session_log_append(rec)     Queue a completed SessionRecord (never blocks on flash).
4. session_log_count_unsent()  Number of sessions not yet acknowledged by the app.
5. session_log_get_unsent(i)   Index entry of the i-th unsent session.
6. session_log_read(id)        Read + CRC-check one record by session id.
7. session_log_mark_sent(id)   Mark a session as received by the app (appends a 'SACK' marker).
8. session_log_purge_sent()    Drop sent records and compact the log.
9. session_log_max_id()        Highest stored id (keeps new ids unique).
10. session_record_fill_therapy() Build therapy pattern events from the played sequence.

## Core algorithm
Session ends -> SessionRecord built (session_stats.cpp)
      ▼
Queued in RAM, appended to /sess_v2.log ~400 ms later
      ▼
App connected? -> SESS_AVAIL {id} hint
      ▼
FETCH_SESSIONS -> S_HDR / S_EV / S_END per session
      ▼
ACK_SESSIONS ok -> 'SACK' markers appended
      ▼
All sent -> log compacted (temp file + rename)

## Safety
- A record counts only if both magics, both ids and the CRC match.
- A torn or damaged tail is cut off by compacting before the next append.
- Compaction writes a temp file first; the old log is only replaced when the
  copy is complete, so a failed rewrite never loses stored sessions.
- When full, the oldest sent record is evicted first, then the oldest unsent.
