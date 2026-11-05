# Session storage & sync — v2

Each completed session (training or therapy, at least 30 s) is stored on the
device as one self-contained record, keyed by a **session id**, and synced to
the app over the single BLE characteristic as JSON notifications.

## Session id

- `u32`, monotonically increasing, shared by training and therapy.
- It is the identity of a session: **dedupe by `(device, id)`**.
- IDs are reserved and persisted when training or therapy starts and are
  retained in the completed record. Sessions discarded before 30 seconds
  still consume an ID, so gaps in IDs are normal.
- A factory reset or counter reset does not reuse ids that are still stored
  on the device. Ids that were already synced and purged may be reused after
  `resetAllSessionCounters()`, so the app should treat a reset device as a
  fresh history (or also compare `ts`).

## Live training packets

Both live posture (`t: "L"`) and training telemetry (`t: "T"`) include
`id`, the active training session ID. `L.id` is 0 when no training session
is active; this field does not identify therapy sessions.

The app should associate provisional training analytics with `(device, id)`.
When the completed session is fetched, replace those provisional analytics
with the complete device record for the same key instead of inserting another
session. Live packets do not replay offline events. Sessions shorter than
30 seconds have no completed record; provisional entries must account for this.

## Live notification (session just ended)

Sent when the app is connected and authorized, right after a session is stored:

```json
{"t":"SESS_AVAIL","id":42,"ty":1,"n":3}
```

| field | meaning |
|---|---|
| `id` | id of the session that just ended |
| `ty` | 1 = training (posture), 2 = therapy |
| `n`  | unsent sessions now on the device |

The app can respond with `FETCH_SESSIONS` straight away.

## Transfer

App → device: `{"cmd":"FETCH_SESSIONS","seq":N}`

Device → app, one notification each, every packet carries the transfer id `x`
and a packet sequence number `q` (0, 1, 2, … across the whole transfer):

```
SESS_HDR                          transfer header
  S_HDR   id=A                    session A header
  S_EV    id=A  i=0..n-1          session A events (0+ chunks)
  S_END   id=A                    session A end marker + totals
  S_HDR   id=B …
  …
SESS_END                          transfer end
```

### `SESS_HDR`
```json
{"t":"SESS_HDR","x":255649,"n":2,"total":2}
```
`n` = sessions in this transfer (at most 40), `total` = unsent on device.
`n:0` means nothing to send. If `total > n`, fetch again after the ACK.
`"err":"MTU_TOO_SMALL"` means the ATT MTU is below 140.

### `S_HDR` — session header
```json
{"t":"S_HDR","x":255649,"q":0,"id":42,"ty":1,"ts":1761480000,"sy":1,"tr":0,"ec":2}
```
| field | meaning |
|---|---|
| `id` | session id |
| `ty` | 1 = training, 2 = therapy |
| `ts` | start time, Unix epoch seconds UTC (0 = device time unknown) |
| `sy` | 1 = `ts` came from a fresh time sync, 0 = estimated |
| `tr` | 1 = more events happened than were stored (training keeps the first 64 slouches; totals still cover the whole session) |
| `ec` | number of `S_EV` packets that follow |
| `err` | present (`"GONE"`) only if the record was evicted mid-transfer; `S_END` follows with zeros |

### `S_EV` — events
Training: each element is one slouch period `[start, dur]`, both seconds from
session start. A slouch still active at the end lasts until the session ended.
```json
{"t":"S_EV","x":255649,"q":1,"id":42,"ty":1,"i":0,"n":2,"e":[[12,30],[95,8]]}
```
Therapy: each element is one pattern run `[pattern, start, dur]`.
```json
{"t":"S_EV","x":255649,"q":5,"id":43,"ty":2,"i":0,"n":1,"e":[[4,0,120],[9,120,120],[2,240,60]]}
```
`i` = chunk index, `n` = chunk count for this session.

### `S_END` — session end marker + totals
Training:
```json
{"t":"S_END","x":255649,"q":3,"id":42,"ty":1,"d":600,"sc":7,"bad":95,"good":505}
```
| field | meaning |
|---|---|
| `d` | total session duration, s |
| `sc` | total slouches |
| `bad` | total time in bad posture, s |
| `good` | total time in good posture, s (`d - bad`) |

Therapy:
```json
{"t":"S_END","x":255649,"q":6,"id":43,"ty":2,"d":300,"pc":3}
```
`pc` = patterns played.

A session is complete in the app once `S_HDR`, all `n` `S_EV` chunks and
`S_END` for that `id` have arrived.

### `SESS_END` — transfer end
```json
{"t":"SESS_END","x":255649,"pk":7,"n":2}
```
`pk` = packets sent (`q` runs 0..pk-1).

### ACK
App → device, echoing `x` as a number:
```json
{"cmd":"ACK_SESSIONS","xfer":255649,"ok":true}
```
Missing packets: `{"cmd":"ACK_SESSIONS","xfer":255649,"nack":[3,4]}` (at most 16
`q` values). The device resends exactly those packets, then `SESS_END` again;
ACK again. Nothing is marked sent until an `ok` ACK arrives, and without an
ACK (timeout 10 s) the same sessions are sent on the next fetch.

## On-flash record (`/sess_v2.log`)

Append-only, little-endian, packed.

| part | bytes | fields |
|---|---|---|
| header | 18 | `'SREC'` u32 · ver u8 (=1) · type u8 · flags u8 (bit0 time synced, bit1 events truncated) · rsv u8 · session_id u32 · start_ts u32 · body_len u16 |
| training body | 2 + 4n + 8 | count u16 · n × {start u16, dur u16} · total u16 · slouches u16 · bad u16 · good u16 |
| therapy body | 1 + 5n + 4 | count u8 · n × {pattern u8, start u16, dur u16} · total u16 · patterns u8 · rsv u8 |
| footer | 12 | `'SEND'` u32 · session_id u32 · crc32 (header + body) u32 |

An ACK appends a 12-byte `'SACK'` · session_id · crc32 marker. At boot a
record is only accepted if both magics, both ids and the CRC agree; damaged
records are dropped and the file is compacted. The log keeps at most 120
records / 8 KB of records; beyond that the oldest sent (else oldest) record is
evicted. Legacy `/sessions.log` + `/sess_ev.dat` are migrated once on first
boot of this firmware and then deleted.
