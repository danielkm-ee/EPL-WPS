# Preamble
This file is a description of the firmware.ino operation.

# Components

## Context Manager
Manages device state, current runtime states include:
- STARTUP
- PERIPHERAL_MANAGEMENT
- FAULT
- OPERATING
- IDLE

### STARTUP state description
Device is in the setup() function, is working to initialize pin types and pin states, construct initial variables, initalize drivers for i2c, serial, and ADCs.

## Fault Manager

## Boost Converter Interface

## Power Management Interface

## Output Stage Drive

## Command-Line Interface

## Logging (telemetry)

