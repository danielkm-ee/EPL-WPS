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

# Pulling things apart...
First things first, I'll need to spend some time debloating the main firmware file. I see many cases where variables are defined and used once, places where a collecton of enums are used which can be replaced with a single struct. 
for example:

## Potential for typedefines and structs
One example of a useful typedef struct
```
// Device State Variables
volatile DeviceState deviceState = STARTUP;             // Current device state
volatile DeviceState oldDeviceState = STARTUP;          // Previous device state, used for temporary state storage
volatile ModeOfOperation modeOfOperation = EDM_ISOFREQUENCY_MODE;  // Current mode of operation
volatile FaultStateType FaultStateType = PMM_FAULT_TYPE;// Current fault type
volatile bool modePreparationComplete = false;          // Flag to track if mode preparation is complete
volatile bool isInFaultState = false;                   // Flag indicating if device is in fault state
volatile bool enablePortStatus = false;                 // Status of the enable port

```

Could be a typedef:
```
typedef struct DeviceStatus {
    DeviceState deviceState;
    DeviceState oldDeviceState;
    ModeOfOperation modeOfOperation;
    FaultStateType faultStateType;
    bool isInFaultState;
    bool enablePortStatus;
} DeviceStatus_t;
```
Paired with an initialization function to set the initial values.

```
volatile DeviceStatus_t deviceStatus;
initDeviceState(deviceStatus);
```

Cutting away at things like this will really help me parse things out across multiple files, and clear up the operation of the main file.

## Fault handlers are doing too much
The fault handling could likely be much simpler, and isr handlers can be much shorter. Each fault hander seems to (1) take all of the necessary steps to handle a fault during the ISR, including waiting for serial communications, and (2) execute a similar order of steps with much lines in common. Here's an example:
```

void handlePowerOutOfRangeFault() {
    // Function to handle power out of range faults

    // Disable the output stage
    disableOutputStage();

    // Set the fault type
    FaultStateType = POWER_OUT_OF_RANGE_FAULT;
    // Set the device state to FAULT
    deviceState = FAULT;

    // Write feedback port to 0% duty cycle
    setEDMFeedbackDutyCycle(0.0f);
    // Turn off status LED 
    gpio_put(STATUS_LED, false);
    // Serial print that the function is starting
    Serial.println("Power Out of Range Fault");

    while (true) {
        // If serial is available, process commands
        if (Serial.available()) {
            String command = Serial.readStringUntil('\n');
            processCommand(command);
        }
    }
}

void handleHighVoltagePhaseSetupFault() {
    // Function to handle high voltage phase setup faults

    // Clear the GPIO interrupt status 
    gpio_acknowledge_irq(BOOST_CONVERTER_PGOOD_FAULT, GPIO_IRQ_EDGE_FALL);
    // Disable the output stage
    disableOutputStage();

    // Set the fault type
    FaultStateType = HIGH_VOLTAGE_PHASE_SETUP_FAULT;
    // Set the device state to FAULT
    deviceState = FAULT;

    // Write feedback port to 0% duty cycle
    setEDMFeedbackDutyCycle(0.0f);
    // Turn off status LED 
    gpio_put(STATUS_LED, false);
    // Serial print that the function is starting
    Serial.println("High Voltage Phase Setup Fault");

    while (true) {
        // If serial is available, process commands
        if (Serial.available()) {
            String command = Serial.readStringUntil('\n');
            processCommand(command);
        }
    }
}
```
BOTH of these functions do the exact same things:
1. acknowledge the interrupt
2. disable the output stage
3. set the device state to FAULT
4. set the feedback duty cycle to 0
5. turn off the status LED
6. print the fault type
7. open a serial port and wait for commands

Another way to do this would be to create handlers that do three (maybe two) things:
1. Acknowledge the ISR request
2. set the device into fault status
3. set the fault type

We could then check for the fault status in main{}, and handle the specific fault according to the fault type. Steps 4, 5, 6, and 7 could then exist in main{}.

## Unused variables, variables that dont need to be global
I have no idea why they've made so many variables global, and in many cases they've declared them as 'volatile', hinting that they are aware they are temporary.
I've found some of my own but I figured I'd ask Gemini again to pull out a list of variables I can potentially remove. Here's what it gave me:
```
1. Unused Variables

These variables are declared and sometimes initialized but are never read from or written to in the logic provided, meaning they can be safely removed to save memory and reduce clutter.

    dischargeRateCalculationInterval_MICROS: This is declared and intended to be calculated from the machining frequency, but it is never actually assigned a value or used in timing logic.

    EDMIsofrequencyModeWrapValue : The code uses pwmWrapValue  inside the setupOutputPWM function for active calculations. These mode-specific "WrapValue" variables appear to be redundant placeholders.

    EDMIsofrequencyModeLevelValue: Similar to above; the active code uses pwmLevelValue.

    EDMIsofrequencyModeHighVoltageLevelValue : The code uses pwmHighVoltageLevelValue  for the actual PWM configuration.

    lastDischargeSuccessRateCalculationTime: While intended for tracking timing, the code does not use this variable to throttle or trigger the rate calculation.

2. Variables That Don't Need to be Global

These variables are used strictly within a single function. In a modular project, these should be moved to local scope (declared inside the function) to prevent "namespace pollution" and potential accidental modification by other parts of the program.

    pmmCalibrationSum : This is only used inside setupPowerManagementModule()  to calculate the average offset during startup. It should be a local long within that function.

    inrushStartTime_MS : This is used only within setupPowerManagementModule()  to handle the 500ms blocking delay during startup.

    serialInitStartTime_MS : Used only within setupSerialCommunication()  to manage the serial timeout.

    outputCurrentSensorCalibrationSum : Used only inside calibrateOutputCurrentSensor()  for the initial offset calculation.

    pwmWrapValue, pwmLevelValue, and pwmHighVoltageLevelValue : These are calculated at the beginning of setupOutputPWM()  and immediately used to set the PWM registers. Since they aren't used for real-time adjustments in the loop(), they should be local to the setup function.

    outputOvercurrentThresholdVoltage and outputOvercurrentThresholdLevel : These are intermediary calculation steps used only inside setupOutputPWM().

    pmmISENSEAdcValue : This stores a temporary raw reading in updateInputCurrentSensorBufferWithNewReading(). The final current value is stored in inputCurrentReading, making the raw ADC global variable unnecessary.

    inputCurrentBufferSum : This is a temporary accumulator used in updateDeviceStatistics()  to calculate a running average. It can be a local variable initialized to 0.0f at the start of that function.
```

