# Training intensity over BLE

Add `intensity` to the existing `TRAINING_START` command:

```json
{"cmd":"TRAINING_START","sub_mode":"Instant","difficulty_angle":25,"delay_ms":5000,"intensity":140}
```

`intensity` is an integer PWM duty from **0 to 255**, applied directly during
the ON part of training alert pulses. For example, 250 uses PWM 250, 140 uses
PWM 140, and 0 mutes training alerts while posture tracking continues.
Existing alert delays, pulse timing, and No alerts behavior still apply.

Omitting the field uses the default PWM 140. Invalid values
return `BAD_INTENSITY` without applying the training settings. Sending another
valid `TRAINING_START` while training is active updates the settings without
restarting the session. The `T` telemetry packet includes `intensity`.

The setting is held in RAM and resets to 140 on reboot. Therapy intensity levels
and calibration/status haptics are independent of this setting.
