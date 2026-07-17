[BLE RX] {"cmd":"ACK_SESSIONS","nack":[0,1,2,3,4,5,6,7,8],"xfer":null}
Two things are wrong with that packet, both app-side:

"xfer":null — the ACK handler (bluetooth.cpp:1537) requires the app to echo the transfer ID it got in SESS_HDR/SESS_END ("x":255649). extractJsonIntField can't parse null, so the firmware rejects the whole ACK with BAD_XFER. syncAckReceived is never set, the FSM stays in WaitAck, times out (ack_timeout), and — by design — nothing is marked sent. That's exactly why every FETCH_SESSIONS in your log returns the same 5 sessions with total:5 — the firmware is deliberately keeping them because it never got a valid ACK.

"nack":[0,1,2,3,4,5,6,7,8] — the app is negative-acking all nine packets, i.e. claiming it received none of them, even though the log shows all 9 went out (SESS_END pk:9 confirms the firmware pushed seq 0–8 with successful notifies). Even if xfer had been correct, this wouldn't ACK anything — it would trigger a full retransmit of the window. A successful handoff must be {"cmd":"ACK_SESSIONS","xfer":255649,"ok":true} (empty/absent nack).

So: the app never actually ACKed — it sent what the firmware protocol reads as "invalid ACK", and the sessions were intentionally retained. The app not showing the session is consistent: its receive pipeline apparently didn't register any of the 9 packets (hence nack-all), which points at the app's parser not tracking the q sequence numbers or dropping the packets before parsing.

What to fix in the app
Echo the transfer ID: read x from SESS_HDR (or SESS_END) and send it back as a number: "xfer":255649, never null.
On success send "ok":true with no nack; only list genuinely missing q values in nack (and then expect a re-SESS_END after retransmission, followed by another ACK round).
Dedupe received packets by q and ignore unparseable notifications.
One small firmware observation, not a bug: because the app's ACK had no "seq" field, the firmware never told it about the BAD_XFER rejection (bluetooth.cpp:1556 only acks when seq is present) — the app just got silence. If you add "seq" to the app's commands, it'll get explicit ok/error feedback for every command, which would have made this failure visible immediately.