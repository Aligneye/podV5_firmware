Ye sab required header files include ki gayi hain jisse Bluetooth module dusre modules ke functions use kar sake.
bluetooth.h → Bluetooth related declarations
calibration.h → Calibration functions
therapy.h → Therapy mode control
training.h → Training mode control
device_time.h → Device time synchronization
session_stats.h → Session details
bluefruit.h → Nordic Bluefruit BLE library

Gloabal variables:
1. int therapyIntensityLevel = 2; : Ye variable current therapy intensity ko store karta hai.
Default value 2 hai jo Mid intensity ko represent karti hai.
2. extern RTTStream rtt; : Ye RTT debugging object hai.
Firmware me jab bhi koi Bluetooth event hota hai to debugging messages RTT terminal par print kiye jate hain.
3. static bool connected=false; : Ye check karta hai ki mobile app connected hai ya nahi.
true → Phone connected
false → Phone disconnected
Isi variable ki help se decide hota hai ki JSON data send karna hai ya nahi.
4. static bool bleInitialized=false; : Bluetooth ko baar-baar initialize hone se rokne ke liye use hota hai.
Agar ek baar BLE setup ho gaya hai to dubara initialization nahi hogi.
Ye memory aur processing dono save karta hai.
5. static BLEService gService(BLE_SERVICE_UUID); : Ye Bluetooth Service object create karta hai.
Mobile app isi service ke through device ke saath communicate karti hai.
Har BLE device ke andar ek ya multiple services hoti hain.
6. static BLECharacteristic gCharacteristic(BLE_CHARACTERISTIC_UUID); : Characteristic BLE ka data channel hota hai.
Isi characteristic ke through:
  App command bhejti hai.
  Device JSON data send karta hai.

📌Function-1: startAdvertising()
Purpose: Bluetooth advertising start karna.
Is function ke baad nearby mobile phones device ko scan karke detect kar sakte hain.
Working:
Sabse pehle purani advertising stop hoti hai.
Bluefruit.Advertising.stop();
Taaki koi old advertisement background me na chale.
Fir previous advertising data clear hota hai.

Bluefruit.Advertising.clearData();
Bluefruit.ScanResponse.clearData();
Ye ensure karta hai ki naya fresh advertising packet create ho.
Fir advertising flags add kiye jate hain.

Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
Ye batata hai ki device BLE General Discoverable Mode me hai.
Matlab nearby devices ise search kar sakte hain.

Bluefruit.Advertising.addTxPower();
Advertising packet me transmission power bhi include karti hai.
Ye connection quality improve karne me help karta hai.

Bluefruit.Advertising.addService(gService);
Is line se BLE service advertisement packet me add hoti hai.
App ko pata chal jata hai ki ye device kis service ko support karta hai.

Bluefruit.ScanResponse.addName();
Ye device ka naam scan response me add karta hai.
Isi wajah se mobile app me device ka naam show hota hai.

Bluefruit.Advertising.restartOnDisconnect(true);
Bahut important line hai.
Agar user disconnect kar de to advertising automatically restart ho jayegi.
User ko manually restart nahi karna padega.

Bluefruit.Advertising.setInterval(32,244);
Advertising interval set karta hai.
Fast interval connection jaldi establish karta hai.Slow interval battery save karta hai.
Firmware automatically dono ka balance maintain karta hai.

Bluefruit.Advertising.setFastTimeout(30);
30 seconds tak fast advertising chalegi.
Uske baad automatically slow advertising start ho jayegi.Ye battery optimization ke liye kiya gaya hai.

Bluefruit.Advertising.start(0);
Advertising start ho jati hai.0 ka matlab hai advertising continuously chalegi jab tak manually stop na ki jaye.

📌Function 2 : onBleConnect()
Purpose: Ye callback function hai.Jab mobile app successfully connect hoti hai tab ye automatically execute hota hai.
Working
connected=true;
Ye variable update karta hai ki device connected hai.Ab firmware JSON data bhejna start kar sakta hai.

rtt.println("BLE: Connected");
Debugging terminal par message print hota hai.

📌Function 3 : onBleDisconnect()
Purpose: Jab mobile app disconnect hoti hai tab ye callback automatically call hota hai.
Working:
connected=false;
Device ko bataya jata hai ki ab koi connection active nahi hai.Ab JSON notifications stop ho jayengi.

rtt.println("BLE: Disconnected");
Debug terminal par disconnect message print hota hai.

startAdvertising();
Bahut important line hai.Disconnect hone ke turant baad device dubara advertising start kar deta hai.Is wajah se user bina restart kiye fir se connect kar sakta hai.

📌Function 4 : applyTrainingTiming()
Purpose: Training mode me posture alert kis style me aayega usko set karna.
Working:
value.trim();
Extra spaces remove ho jate hain.

value.toUpperCase();
Input ko uppercase me convert kiya jata hai.

Example:instant,Instant,INSTANT
Teenon ko same treat kiya jayega.Agar command ho

POSTURE_TIMING=INSTANT
to
trainingSubModeIndex=TrainingAlertStyle::Instant;
Instant alert mode activate ho jayega.

Agar command ho,POSTURE_TIMING=DELAYED
to delayed posture alert mode activate ho jayega.

Agar app AUTOMATIC bhejti hai to firmware currently usse Instant mode me convert kar deta hai.
📌Function 5 : applyTherapyDurationMinutes()
Purpose:Therapy session ki duration set karna.
Working:
value.trim();
Fir integer me convert kiya jata hai.int mins=value.toInt();
Example: THERAPY_DURATION_MIN=20 to mins = 20 ban jayega.Agar value invalid ho
if(mins<=0) return; to function wahi stop ho jayega.Ye invalid commands ko ignore karta hai.
Agar duration 10 hai to therapySubModeIndex=0; Agar 20 hai to therapySubModeIndex=1;Agar
30 hai to therapySubModeIndex=2;
rtt.printf(...)
Debug terminal par received duration print hoti hai taaki developer verify kar sake ki command successfully receive hui hai.
📌Function 6 : applyMode()
Purpose:
Ye function Bluetooth se receive hui mode command ko process karta hai aur device ko Tracking, Training ya Therapy mode me switch karta hai.
Working:
Check karta hai ki calibration chal rahi hai ya nahi.
Received command ko clean aur uppercase me convert karta hai.
MODE=TRACKING par Tracking mode activate hota hai.
MODE=TRAINING par Training mode activate hota hai.
MODE=THERAPY par Therapy mode activate hota hai.
Example: setDeviceMode(MODE_THERAPY); Ye line device ko Therapy mode me switch karti hai.

📌Function 7 : applyCalibrationControl()
Purpose: Bluetooth command ke through calibration ko start ya cancel karna.
Working
CALIBRATION=START par calibration start hoti hai.
CALIBRATION=CANCEL par calibration stop hoti hai.
Agar calibration already same state me ho to duplicate command ignore kar di jati hai.

📌 Function 8 : applyAction()
Purpose: Different BLE action commands ko execute karna.
Working;
ACTION=CALIBRATE → Calibration start karta hai.
ACTION=CALIBRATE_CANCEL → Calibration cancel karta hai.
ACTION=PROFILE_CLEAR → Saved profiles delete karta hai.
Ye function multiple actions ko ek hi jagah handle karta hai.

📌 Function 9 : applyTimeSync()
Purpose: Mobile app ka current time device ke saath synchronize karna.
Working:
Received timestamp ko integer me convert karta hai.Valid hone par device ka internal time update karta hai.Debug ke liye RTT par log print karta hai.
Important Line
setDeviceTime(epoch);
Ye line device ka current time set karti hai.

📌 Function 10 : applyTZOffset()
Purpose: Device ka timezone update karna.
Working:
App se timezone offset receive karta hai.Value ko integer me convert karta hai.Device ke timezone offset ko update karta hai.
Example: setDeviceTZOffset(tz); 
Ye line timezone offset apply karti hai.

📌 Function 11 : applyTherapyIntensity()
Purpose: Therapy vibration intensity change karna.
Working:
Bluetooth se intensity level receive karta hai.
Sirf valid values (1–3) accept karta hai.
Current therapy intensity update karta hai.
Important line: therapyIntensityLevel = level;
Ye line therapy intensity ko update karti hai.

📌 Function 12 : applyDifficultyDegrees()
Purpose: Bad posture detect karne ka angle threshold set karna.
Working: 
Bluetooth se degree value receive karta hai.
Valid range (5°–60°) check karta hai.
Threshold update karta hai.

📌 Function 13: applyProfileSelection()
Purpose:Ye function Bluetooth command ke through calibration profile ko select, reset ya clear karta hai.
Working:
PROFILE=CLEAR par saare saved profiles delete ho jate hain.
PROFILE=DEFAULT par default profile select hoti hai.
Profile number receive hone par us profile ko activate karta hai.
Agar profile name receive ho to us naam ka profile search karke select karta hai.
Important line:
selectCalibrationProfile(i);
Ye line selected calibration profile ko activate karti hai.

 📌 Function 14:parseAndApplyBleCommand()
Purpose:Ye Bluetooth module ka main command parser hai jo app se receive hui command ko read karke uske according function call karta hai.
Working:
Complete payload receive karta hai.
Command ko `;` ke basis par alag karta hai.
Har command ko `key=value` format me split karta hai.
Key ke according related function call karta hai.
Last me agar mode command ho to `applyMode()` execute karta hai.
Important Line:
applyMode(requestedMode);
Ye line received mode command ko process karti hai.

📌 Function 15:onCharacteristicWrite()
Purpose:
Jab mobile app Bluetooth ke through data bhejti hai tab ye callback function automatically execute hota hai.
Working:
 Received byte data ko String me convert karta hai.
 Debug log print karta hai.
 Complete payload ko `parseAndApplyBleCommand()` function ko bhej deta hai.
 Important Line:
 parseAndApplyBleCommand(payload);
 Ye line received Bluetooth command ko process karne ke liye parser ko call karti hai.

📌 Function 16:bluetoothSetup()
 Purpose:Bluetooth module ko initialize karna aur communication ke liye ready karna.
Working:
Bluefruit library initialize karta hai.
Device name aur TX power set karta hai.
BLE Service aur Characteristic create karta hai.
Read, Write aur Notify properties set karta hai.
Connect/Disconnect callbacks register karta hai.
Last me advertising start kar deta hai.
Important Line:
startAdvertising();
Ye line Bluetooth advertising start karti hai jisse mobile app device ko detect kar sakti hai.

📌 Function 17:bluetoothLoop()
Purpose:
Ye function continuously device ka live data collect karke mobile app ko Bluetooth ke through bhejta hai.
 Working:
Fixed interval par execute hota hai.
Sensor values aur posture angle calculate karta hai.
 Current mode aur profile information collect karta hai.
JSON packet create karta hai.
Agar device connected ho to data app ko notify karta hai.
Important Line:
pCharacteristic->notify(jsonBuffer);
Ye line live JSON data mobile application ko send karti hai.

 📌 Function 18:bluetoothStartAdvertising()
 Purpose:
Bluetooth advertising manually start karna.
 Working:
 Debug message print karta hai.
  `startAdvertising()` function call karke advertising start karta hai.
Important Line:
startAdvertising();
Ye line device ko discoverable banati hai.

 📌 Function 19:bluetoothStopAdvertising()
 Purpose: Bluetooth advertising stop karna.
 Working:
 Debug message print karta hai.
 Current advertising process ko stop kar deta hai.
 Important Line:
Bluefruit.Advertising.stop();
Ye line Bluetooth advertising ko stop karti hai.

📌 Function 20:bluetoothIsConnected()
 Purpose: Check karna ki mobile app Bluetooth se connected hai ya nahi.
 Working:
`connected` variable ki current value return karta hai.
 Dusre modules is function ki help se connection status check kar sakte hain.
 Important Line:
return connected;
Ye line Bluetooth connection status return karti hai.

📌 Function 21: bluetoothRequestCalibrationStart()
 Purpose: Bluetooth ke through calibration process start karna.
 Working:
 Directly calibration module ko start request bhejta hai.
 Important Line:
calibrationRequestStart();
Ye line calibration process initiate karti hai.

 📌 Function 22: notifyNewSessionStored()
 Purpose:Ye future use ke liye placeholder function hai.
 Working:
 Abhi is function me koi implementation nahi hai.
 Future me naya session save hone par notification bhejne ke liye use kiya ja sakta hai.

 📌 Overall Summary:
`bluetooth.cpp` project ka communication backbone hai. Ye Bluetooth initialize karta hai, mobile app se commands receive karta hai, unhe process karta hai aur posture, therapy aur calibration ka live data JSON format me mobile app ko send karta hai. Is file ki wajah se hardware aur mobile application ke beech real-time communication possible hota hai.