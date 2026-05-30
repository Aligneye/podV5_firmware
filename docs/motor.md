Aaiye Rahul ke example ke sath src/motor.cpp ke pure code ko 8 Parts mein divide karke samajhte hain ki jab Rahul apne device ko use karega, tab kaun si line kab aur kaise execute hogi:

Part 1: Imports aur State Tracker Variables
Line Range: Lines 1 to 28
Behind the Scenes: Yeh hisab-kitab rakhne wali dairy hai jo Rahul ke device ke settings ko track karti hai.
g_dutyWanted: Rahul ke posture alert ya therapy ke anusar device kitna vibrate hona chahta hai.
g_dutyApplied: Is waqt motor actually kis level par chal rahi hai.
g_overrideUntilMs: Agar Rahul button dabaye toh haptic feedback kab tak chalega, uska time store hota hai.
Part 2: Custom TIMER1 Controls (Currently Retired/Reserved)
Line Range: Lines 31 to 83
Behind the Scenes: Yeh low-level configuration code hai jo timer cycles ke ticks ginta hai. Filhal ise bypass kiya gaya hai taaki nRF52 chip par safe aur standard PWM operation ho sake.
Part 3: Wake Up & Booting (Initialization)
Line Range: Lines 107 to 114 (motorSetup)
Rahul's Action: Rahul subah uthkar apne Aligneye Pod ko power ON karta hai.
Kaise chize work hoti hain:
Rahul ke switch ON karte hi system motorSetup() call karta hai.
Line 108-109: pinMode pin no. 17 (PIN_MOTOR) ko active OUTPUT set karta hai aur motor ko switch off (LOW) kar deta hai taaki startup par bina wajah motor na kaanpe.
Line 110-113: Saari purani values aur overrides ko 0 par lock kar deta hai.
Part 4: Normal Duty Cycle Demand (Vibration Alert Request)
Line Range: Lines 116 to 123 (motorSetDuty)
Rahul's Action: Rahul normal training mode par hai. Woh jhukta hai (bad posture) aur app detect karti hai ki use vibrate karke alert karna chahiye.
Kaise chize work hoti hain:
Bad posture hone par system motor ko vibrate karne ke liye motorSetDuty(VIB_INTENSITY_MAX) (full speed) call karta hai.
Line 117: Target target speed g_dutyWanted = 255 diary mein likh li jati hai.
Line 119: System check karta hai ki Rahul ne abhi koi button toh nahi dabaya (VIP priority override check). Agar sab normal hai, toh:
Line 122: applyDuty(255) call ho jata hai.
Part 5: The Speed Control Engine (Apply Duty Cycle)
Line Range: Lines 85 to 98 (applyDuty)
Rahul's Action: Rahul app se therapy mode start karta hai aur vibration intensity ko "Low" select karta hai.
Kaise chize work hoti hain:
Jab applyDuty call hota hai, toh yeh check karta hai ki kya Rahul Therapy mode mein hai (Line 86).
Agar intensity Low hai, toh mathematical scaling apply hoti hai: scale = 0.70f.
Jo bhi intensity input aayi thi (maan lijiye 200), use scale-down karke 200 * 0.7 = 140 kar diya jata hai (Line 90).
Line 97: analogWrite(PIN_MOTOR, 140) call hota hai aur motor thodi dheemi aur soothing speed par vibrate hoti hai.
Part 6: Triggering Urgent Alerts (Override Request)
Line Range: Lines 125 to 131 (motorOverrideDuty)
Rahul's Action: Therapy chalne ke dauran, Rahul double-click karke therapy duration badalna chahta hai. Click karte hi click confirmation buzz (haptic feedback) hota hai.
Kaise chize work hoti hain:
Rahul jab button press karega, toh system motorOverrideDuty(100, 200) (yaani 100 ki speed par 200 milliseconds) call karega.
Line 128: Normal speed ko temporarily side mein rakh kar g_overrideDuty = 100 set kiya jata hai.
Line 129: g_overrideUntilMs = current time + 200ms register kiya jata hai.
Line 130: Normal therapy vibration ko rokk kar immediately click haptic applyDuty(100) execute ho jata hai.
Part 7: Checking Override Expiration (Timekeeper)
Line Range: Lines 100 to 105 (overrideActive)
Rahul's Action: Click karne ke baad 200ms beet chuke hain.
Kaise chize work hoti hain:
overrideActive() current millisecond check karta hai.
Jab tak time poora nahi hota, yeh system ko batata hai ki "Abhi Rahul ka button feedback khatam nahi hua hai" (return true).
Jaise hi 200ms poore ho jaate hain, yeh duration ko reset (0) karke return false deta hai.
Part 8: Main Loop Monitor (The Constant Watcher)
Line Range: Lines 133 to 141 (motorUpdate)
Rahul's Action: Device har microsecond loop chala raha hai jab Rahul use pehn kar baitha hai.
Kaise chize work hoti hain:
Rahul ka device continuous chalta rahega toh loop() ke andar motorUpdate() har samay call hota hai.
Line 135: Yeh check karta hai ki kya button press ka vibration (overrideActive) chal raha hai?
Lines 136-137: Agar chal raha hai, toh usi button feedback speed ko chalte rehne deta hai aur normal program ko bypass kar deta hai.
Lines 139-140: Jaise hi 200ms beet jaate hain, yeh automatic wapas normal routine speed g_dutyWanted (jaise therapy intensity 140) par Sheru ko chala deta hai. Rahul ko bina kisi glitch ke seamless feedback aur therapy ka mix milta hai.