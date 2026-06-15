Overview:
The therapy.cpp file is responsible for managing the therapy mode of the posture tracking device.
When therapy mode starts, the module generates a sequence of vibration patterns and controls the vibration motor accordingly. It also tracks therapy duration, switches between different therapy patterns, and ends the session automatically when the configured time is completed.

This module acts as the therapy engine of the firmware.

This module is responsible for:
Starting therapy sessions
Stopping therapy sessions
Managing therapy duration (10/20/30 min)
Generating therapy pattern sequence
Controlling vibration motor
Switching between different therapy patterns
Tracking current and next therapy pattern
Providing therapy status to other modules

 # important variables

 Variable	Purpose
therapyState->	Current therapy state
therapyStartMs->	Therapy start time
therapyDurationMs->	Total therapy duration
patternSequence[]->	Stores therapy pattern order
totalPatterns->	Total patterns in current session
currentPatternIndex->	Currently executing pattern
patternsInitialized->	Checks whether pattern sequence is ready

# Function Explanation
1. 📌 durationForSubMode()
Purpose: Returns therapy duration based on selected mode.
Working:
Mode 0 → 10 minutes
Mode 1 → 20 minutes
Mode 2 → 30 minutes

2. 📌 getTherapyTotalPatternCount()
Purpose: Returns total number of therapy patterns in the current session.

3. 📌 getTherapyUniquePatternCount()
Purpose: Returns the number of unique therapy patterns used.
Working: Ignores duplicate patterns and counts only distinct patterns.

4. 📌 getTherapyPatternSequence()
Purpose: Returns the complete therapy pattern sequence.
Working: Copies the generated pattern order into the provided buffer.

5. 📌 getPatternNameByIndex()
Purpose: Returns the name of a therapy pattern.
Example
0 → Muscle Act
1 → Rev Ramp
2 → Ramp
3 → Wave

6. 📌 initializePatternSequence()
Purpose: Generates the therapy pattern sequence.
Working:
Calculates total patterns.
Fills initial patterns sequentially.
Remaining patterns are selected randomly.
Prints pattern sequence in RTT log.

This function prepares the complete therapy session before execution.

7. 📌 printTick()
Purpose: Prints therapy progress every second.
Working:
Displays:
Elapsed time
Current pattern
Total patterns
Current pattern name
Next pattern name

Useful for debugging.

## Therapy Pattern Functions
The following functions generate different vibration effects for the motor.

Function	Purpose
patternMuscleActivation() ->	Gradually increases intensity
patternReverseRamp() ->	Gradually decreases intensity
patternRampPattern()->	Up and down ramp pattern
patternWaveTherapy() ->	Wave pulse vibration
patternSlowWave() ->	Slow changing vibration
patternSinusoidalWave() ->	Smooth sine wave vibration
patternTriangleWave() ->	Triangle shaped intensity
patternDoubleWave() ->	Double layered wave
patternAntiFatigue() ->	Pulse pattern for fatigue reduction
patternPulseRamp() ->	Increasing pulse sequence
patternInstantTripleBase() ->	Triple pulse with base vibration
patternConstTriple() ->	Constant triple pulse
patternExpDoubleSine() ->	Exponential sine waveform
patternBreathingExpSquare() ->	Breathing-like vibration effect

Each function calculates a PWM value and sends it to the vibration motor using motorSetDuty().

9. 📌 executePattern()
Purpose: Executes the selected therapy pattern.
Working:
Uses a switch statement to call the correct pattern function based on the current pattern index.

10. 📌 therapySetup()
Purpose: Initializes the therapy module.
Working:
Initializes motor
Sets therapy state to idle
Clears previous pattern sequence

Called during device startup.

11. 📌 therapyStart()
Purpose: Starts a therapy session.
Working:
Sets therapy duration
Stores start time
Initializes pattern sequence
Resets pattern index
Starts session statistics

12. 📌 therapyStop()
Purpose: Stops the therapy session.
Working:
Stops motor vibration
Sets therapy state to idle
Optionally switches device back to Training mode
Ends therapy statistics

13. 📌 therapyLoop()
Purpose: Main execution loop of therapy mode.
Working:
Checks therapy status
Calculates elapsed time
Switches pattern after fixed interval
Executes current pattern
Stops therapy automatically when duration ends

This is the main control function of the therapy module.

14. 📌 therapyIsRunning()
Purpose: Returns whether therapy is currently active.

15. 📌 therapyGetElapsedMs()
Purpose: Returns elapsed therapy time.

16. 📌 therapyGetRemainingMs()
Purpose: Returns remaining therapy time.

17. 📌 therapyGetCurrentPatternName()
Purpose: Returns the name of the currently running therapy pattern.

18. 📌 therapyGetNextPatternName()
Purpose: Returns the name of the upcoming therapy pattern.

## Core Algorithm
Start Therapy
      ▼

Generate Pattern Sequence
      ▼

Start First Pattern
      ▼

therapyLoop()
      ▼

Execute Current Pattern
      ▼

Pattern Completed?
   │            │
  No           Yes
   ▼            ▼

Continue    Next Pattern
                 ▼

Session Duration Complete?
       No             Yes
        ▼              ▼

 Continue         Stop Therapy


## Summary
therapy.cpp is the therapy controller of the firmware. It manages therapy sessions by generating vibration pattern sequences, controlling the motor, switching between patterns, tracking therapy duration, and ending the session automatically. It provides a structured and dynamic therapy experience while coordinating with other modules like motor control, calibration, Bluetooth, and session statistics.