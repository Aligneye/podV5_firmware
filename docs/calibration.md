# Calibration Module
Ye file device ki posture calibration process ko manage karti hai.Jab user calibration start karta hai, tab ye module accelerometer data collect karta hai, uska analysis karta hai aur stable posture ka reference save karta hai.Calibration successful hone ke baad naya profile create hota hai jo future posture detection me use hota hai.

## Responsibilities
- Start calibration process
- Collect accelerometer samples
- Detect unstable movement
- Calculate average posture
- Reject noisy samples
- Save calibration profile
- Return calibration status

# state machine:
IDLE
 │
 │ Start Calibration
 ▼
HOLD_STILL
 │
 │ 5 seconds sample collection
 ▼
SUCCESS / FAILED
 │
 ▼
Back to Training Mode

## Important Variables

| Variable | Purpose |
|----------|-----------------------------|
| calibState | Current calibration state |
| totalSamples | Total collected samples |
| pendingStart | Start request flag |
| pendingCancel | Cancel request flag |
| samplesX[] | X-axis samples |
| samplesY[] | Y-axis samples |
| samplesZ[] | Z-axis samples |
| lastCalibrationResult | Stores success/failure result |

📌 Function 1 : goToTrainingMode()
Purpose:Calibration complete ya fail hone ke baad device ko automatically Training mode me switch karna.
Working:
Device ON karta hai.
Device mode ko Training mode set karta hai.
Important Line
setDeviceMode(MODE_TRAINING);
Ye line device ko Training mode me switch karti hai.

📌 Function 2 : calibrationFail()
Purpose: Calibration fail hone par required actions perform karna.
Working:
Calibration state reset karta hai.
Result ko failed set karta hai.
Failure vibration trigger karta hai.
Error message RTT par print karta hai.
Device ko Training mode me wapas bhejta hai.
Important Line:
strncpy(lastCalibrationResult, "failed", sizeof(lastCalibrationResult) - 1);
Ye line calibration result ko failed store karti hai.

📌 Function 3 : calibrationSuccess()
Purpose: Successful calibration ke baad calibration data save karna.
Working:
Calibration result ko complete set karta hai.
Average sensor values save karta hai.
Calibration profile create karta hai.
Success vibration trigger karta hai.
Device ko Training mode me switch karta hai.
Important Line: addNextCalibrationProfile();
Ye line naya calibration profile save karti hai.

📌 Function 4 : getLastCalibratedX()
Purpose: Last successful calibration ki X-axis value return karna.
Working:
Stored X calibration value return karta hai.

📌 Function 5 : getLastCalibratedY()
Purpose: Last successful calibration ki Y-axis value return karna.
Working: Stored Y calibration value return karta hai.

📌 Function 6 : getLastCalibratedZ()
Purpose: Last successful calibration ki Z-axis value return karna.
Working: Stored Z calibration value return karta hai.

📌 Function 7 : isLastCalibrationValid()
Purpose: Check karna ki last calibration valid hai ya nahi.
Working: Boolean value return karta hai.
true → Valid
false → Invalid

📌 Function 8 : initCalibration()
Purpose: Calibration module ko initialize karna.
Working:
Sab variables reset karta hai.
Motor stop karta hai.
Pending requests clear karta hai.
Calibration profiles initialize karta hai.
Important Line: initProfiles();
Ye line saved calibration profiles ko initialize karti hai.

📌 Function 9 : handleCalibration()
Purpose: Ye calibration module ka main function hai jo poori calibration process handle karta hai.
Working:
Start aur cancel requests check karta hai.
Sensor samples collect karta hai.
Mean aur Standard Deviation calculate karta hai.
Outliers remove karta hai.
Stable posture hone par calibration complete karta hai.
Excess movement hone par calibration fail kar deta hai.
Important Line: calibrationSuccess(avgX, avgY, avgZ);
Ye line successful calibration complete karti hai.

📌 Function 10 : requestCalibrationStart()
Purpose: Ye function calibration start karne ki request generate karta hai.
Working:
pendingStart flag ko true set karta hai.
Actual calibration handleCalibration() function me start hoti hai.
Important Line: pendingStart = true;
Ye line calibration start request ko register karti hai.

📌 Function 11 : requestCalibrationCancel()
Purpose: Ye function running calibration ko cancel karne ki request bhejta hai.
Working:
pendingCancel flag set karta hai.
cancelCalibration() function call karta hai.
Important Line
pendingCancel = true;
Ye line calibration cancel request register karti hai.

📌 Function 12 : startCalibration()
Purpose: Ye function actual calibration process start karta hai.
Working:
Check karta hai ki calibration already running na ho.
Accelerometer available hai ya nahi verify karta hai.
Therapy mode stop karta hai (agar chal rahi ho).
Motor vibration se calibration start indication deta hai.
Calibration timer aur sample variables reset karta hai.
State ko HOLD_STILL me change karta hai.
Important Line: calibState = CALIB_STATE_HOLD_STILL;
Ye line calibration process ko start state me le jati hai.

📌 Function 13 : cancelCalibration()
Purpose: Running calibration process ko safely stop karna.
Working:
Calibration state reset karta hai.
Motor vibration stop karta hai.
Calibration data invalidate karta hai.
Device ko Training mode me wapas bhejta hai.
Important Line: calibState = CALIB_STATE_IDLE;
Ye line calibration process ko completely stop kar deti hai.

📌 Function 14 : getCalibrationResult()
Purpose: Last calibration ka result return karna.
Working:
Agar calibration chal rahi ho to empty string return karta hai.
Agar result available ho to "complete" ya "failed" return karta hai.
Kuch seconds baad result automatically clear ho jata hai.
Important Line:
return lastCalibrationResult;
Ye line last calibration result return karti hai.

📌 Function 15 : isCalibrating()
Purpose: Check karna ki calibration currently chal rahi hai ya nahi.
Working:
Calibration state check karta hai.
Boolean value return karta hai.
Important Line: return calibState != CALIB_STATE_IDLE;
Ye line batati hai ki calibration active hai ya nahi.

📌 Function 16 : getCalibrationElapsedMs()
Purpose: Calibration start hone ke baad kitna time beet chuka hai, ye return karna.
Working:
Agar calibration active hai to elapsed time calculate karta hai.
Nahi to 0 return karta hai.
Important Line: return (uint32_t)(millis() - stabilityStartTime);
Ye line elapsed calibration time calculate karti hai.

📌 Function 17 : getCalibrationTotalMs()
Purpose: Calibration ki total duration return karna.
Working:
Fixed calibration time return karta hai.
Is project me ye value 5000 ms (5 sec) hai.
Important Line: return CALIB_TOTAL_MS;
Ye line total calibration duration return karti hai.

📌 Function 18 : getCalibrationPhase()
Purpose: Current calibration phase batana.
Working:
Calibration inactive ho to "IDLE" return karta hai.
Start phase me "GET_READY" return karta hai.
Calibration ke dauran "HOLD_STILL" return karta hai.
Important Line: return "HOLD_STILL";
Ye line current calibration phase return karti hai.

📌 Wrapper / Alias Functions: 
Ye functions sirf existing functions ko call karte hain taaki project ke different modules compatible rahen.

calibrationSetup() → initCalibration()
calibrationLoop() → handleCalibration()
calibrationRequestStart() → requestCalibrationStart()
calibrationRequestCancel() → requestCalibrationCancel()
calibrationStart() → requestCalibrationStart()
calibrationStop() → requestCalibrationCancel()
calibrationIsActive() → isCalibrating()

In functions me koi alag logic nahi hai, ye sirf wrapper functions hain.

📌 Function : calibrationMotorActive()
Purpose: Check karna ki calibration ke time motor vibration active hai ya nahi.
Working:
Agar calibration chal rahi ho to true return karta hai.
Success ya failure vibration pulse active ho to bhi true return karta hai.
Baaki cases me false return karta hai.
Important Line: return true;
Ye indicate karta hai ki calibration motor abhi active hai.