this is test

Button.h
Lines 1-5: Inclusions & Header Guards
	#pragma once ensure karta hai ki yeh file compile karte waqt duplicate include na ho.
	Arduino core, debug printing (RTTStream.h), aur external configurations (config.h) ko include kiya gaya hai.



Lines 7-8: Power State
	extern bool deviceOn; ek global boolean variable declare karta hai jo track karta hai ki device active/on hai ya nahi.


Lines 10-19
: Modes Definitions
enum Mode teen main modes ko define karta hai: MODE_TRAINING (0), MODE_THERAPY (1), MODE_OFF (2), aur MODE_COUNT (total 3 modes) ko.
extern const char* modeNames[]; in modes ke readable names ko hold karne wali array ka declaration hai.


Lines 20-26
: Sub-modes Definitions
Training aur Therapy ke liye total number of sub-modes (har ek ke liye 3) define kiye gaye hain.
trainingSubModes aur therapySubModes sub-modes ke names (jaise duration ya alarm setup) ke global declaration hain.


Lines 27-30
: Current States
currentMode, trainingSubModeIndex, aur therapySubModeIndex variable declarations hain jo current active mode aur sub-modes ke status ko save rakhte hain.


Lines 32-35
: Public APIs


buttonSetup()
 aur 

buttonLoop()
 function prototypes hain jinhe core application cycle (main.cpp / standard setup/loop) se call kiya jata hai.
 
 
 . 

src/button.cpp
 Explanation
Yeh file button press patterns (Single Click, Double Click, Long Press/Hold) ko detect karti hai aur uske basis par system states toggle karti hai.



Lines 1-10
: Headers and Logger
Sabhi related modules (calibration, therapy, storage, motor, bluetooth, autoOff) ke headers include kiye gaye hain taaki button action par unhe trigger kiya ja sake.
extern RTTStream rtt; debug logging ke liye setup stream hai.


Lines 11-15
: Name Arrays Configuration
Modes aur sub-modes ke user-friendly strings/labels define kiye gaye hain:
Modes: "Training Mode", "Therapy Mode", "OFF Mode".
Training Sub-modes: "Instant", "Delayed", "No alerts".
Therapy Sub-modes (durations): "10 min", "20 min", "30 min".


Lines 16-20
: Initial State Initialization
Device ke main active state variable set kiye gaye hain (deviceOn = true, default mode = MODE_TRAINING, aur dono sub-mode indices 0).


Lines 22-25
: LED Control Macros
LED control ko clarify karne ke liye macros hain. Kyunki LED common-anode type hai, isliye LOW signal bhejne par LED ON hoti hai aur HIGH par OFF.


Lines 27-37
: Debouncing & State Tracking Variables
btnStablePressed, btnLastDebounceMs, aur btnLastRaw button ke raw electrical signal ko smooth/debounce karne ke variables hain.
pressStartMs, holdTriggered, clickPending, aur lastClickTimeMs single, double click aur long-press events ko identify karne ke liye timestamps track karte hain.
ButtonEvent enum internally batata hai ki button se kaun sa event (None, Single Click, Double Click, Hold) trigger hua hai.


Lines 39-42
: 

playButtonPressHaptic()
Har button press par vibro-motor ko thodi der (45 miliseconds) ke liye low intensity par vibration trigger karta hai taaki user ko tactile/haptic response mile (agar active calibration na chal rahi ho).


Lines 44-47
: 

printCurrentMode()
Ek simple helper function jo RTT logging screen par current mode ka name print karta hai.


Lines 49-87
: 

handleSingleClick()
Jab user ek baar single click karta hai, toh main mode rotate hota hai (Training -> Therapy -> Off -> Training).
Training Mode: Blue LED pin initialize karta hai, Bluetooth start advertising call karta hai, aur activity timer reset karta hai.
Therapy Mode: Therapy session directly start karta hai (therapyStart()).
OFF Mode: Chal rahi therapy session ko stop karta hai aur device ko software power down karta (powerOff()).


Lines 89-112
: 

handleDoubleClick()
Double click karne par sub-modes cycle hote hain:
Training Mode mein: Sub-modes index shift hote hain (Instant -> Delayed -> No alerts).
Therapy Mode mein: Sub-mode index rotate hota hai (durations toggle hoti hain: 10, 20 ya 30 mins). New selection ko EEPROM/Flash storage mein write (storageSaveTherapySubMode()) kiya jata hai taaki next boot par bhi yaad rahe, aur therapy restart ho jaati hai.


Lines 114-125
: 

handleHold()
Long press (hold) action ko handle karta hai. OFF mode mein ise ignore kiya jata hai.
Agar sensor level calibration chal rahi hai toh use cancel karta hai, warna calibration process ko start karta hai.


Lines 127-187
: 

pollButton()
Button processing algorithm:
Button input state read karta hai aur filter out karta hai switch noise (debounce).
Agar button physically press hua (PULLUP pull karne se raw state LOW hoti hai), toh pressStartMs note karta hai aur haptic vibration generate karta hai.
Agar release hua, toh check karta hai ki kya double click time frame ke andar pehle se koi click pending tha. Agar haan, toh EVT_DOUBLE return karega. Nahi toh single click ko queue mein save karke thodi der wait karta hai (DOUBLE_CLICK_GAP_MS).
Agar press-down hold duration HOLD_MS (e.g. 1000ms+) cross kar jaye, toh hold flag trigger karke event ko EVT_HOLD return kar deta hai.


Lines 189-210
: 

buttonSetup()
Button hardware pins configure karta hai (Input standard pullup).
Storage se data retrieve karke saved therapy configurations load karta hai.
State variables aur event indicators clean reset karta hai aur boot notification RTT par send karta hai.


Lines 212-234
: 

buttonLoop()
Har loop iteration par button click evaluate karne ke liye pollButton() chalata hai.
Activity update trigger karta hai taaki system sleep state (auto shutdown) block rahe.
Events switch case ke zariye unke actual handlers (handleSingleClick, handleDoubleClick, handleHold) ko call karte hain.
Therapy autostart logic aur active calibration condition par LED control logic implement karta hai.