# Breaking up the WPS firmware to improve maintenence
Main goal: Break up the drivers for the various hardware devices
	- Consider buck, boost, and output hardware as modules in software
	- Separate state management, error handling, etc into main file

Main sections:
* Sensors -- Current sensor, voltage sensor
* Feedback output -- module for feedback output types

Pin assignments should be separated into a separate `config.h` file to make things easier to read

# Trademark Respects
Another aim of my work on this repository is to pay respects to RackRobotics, Inc. trademarks and attribution.

## Using AI to speed things up:
Prompt:
```
 Hi, I am an undergraduate electrical engineering currently working on a CNC wire EDM for my capstone. We are using Linux CNC for the motion control systems with a Mesa 7i76eu as the step-level controller, and a raspberry pi as the Human Machine Interface. I've got a good deal of experience writing firmware in C and for arduino, and I've found a project for our power supply that could use some improvements to the firmware project. The main issue is that the firmware has been implemented as a single file, making maintenance difficult. I'd much rather have the functions separated into various `.h` and `.c` files that break functionality into sensor drivers, a config.h file, and other modular forms of the project. Could you take a look at this firmware and give me an example of how the firmware could be divided into these modular parts?


I've attached the `.ino` file. [powercoreV3-firmware.ino]
```

Response:
```
Modularizing your firmware is an excellent step for an undergraduate capstone project. Moving from a single, monolithic .ino file to a modular structure significantly improves readability and simplifies debugging.

Based on your powercoreV3-firmware.ino file, here is a recommended structure and examples of how to divide the project into modular components. 
Recommended File Structure

    config.h: Hardware-specific constants, pin definitions, and system parameters. 

    types.h: Global enumerations and structures. 

    sensors.h / .cpp: ADC initialization, current/voltage sensor calibration, and reading logic. 

    pwm_control.h / .cpp: Configuration for output PWM phases and the EDM feedback signal. 

    boost_module.h / .cpp: I2C communication and digital potentiometer (DPOT) management for the high-voltage phase. 

    fault_handler.h / .cpp: Interrupt service routines (ISRs) and safety-related logic. 

    telemetry.h / .cpp: Serial communication and command parsing. 

    powercoreV3-firmware.ino: The main entry point containing setup() and loop(). 
```


I very much agree with the structural decomposition provided by Gemini 3's Thinking Model, so I will be using this as the initial project structure.
