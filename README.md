# EPL's Wire EDM Wire Power Supply (WPS)

A high-efficiency power supply unit designed for Electrical Discharge Machining (EDM) applications. The design is based on the Rack Robotics Powercore V3 and owes much to their engineering efforts.

## EPL Contributions

The EPL has reworked a few aspects of the Rack Robotics Powercore V3 for use in it's CNC-Type Wire EDM. Primarily, we've made use of 2.54mm pin headers in place of the spring-based connectors, or Pogo pins, to simplify construction without the enclosure. Other modifications have been made the the firmware project structure, separating sensor drivers, boost converter, fault handling, and other portions of the project into a modular C++ library.

If you would like to explore or purchase the original PowerCore V3, please vist Rack Robotics at
![The originial PowerCore V3 Repo](https://github.com/Rack-Robotics/Powercore-V3)
and
![Rackrobo.io](https://rackrobo.io/products/powercore-v3)

## Repository Structure

- `circuit-boards/` - KiCAD PCB designs for all modules
- `schematics/` - Schematic PDFs for electronics
- `KiCAD-library/` - Custom component library and 3D models
- `LTSpice/` - Simulation files
- `firmware/` - Arduino firmware for the RP2040 controller


## ⚠️ HIGH VOLTAGE SAFETY WARNING ⚠️

**DANGER - HIGH VOLTAGE PRESENT**
This device generates and outputs high-voltage DC, which could be hazardous. Before operating, building, or servicing this equipment, read and understand all safety warnings.

ELECTRICAL HAZARDS
- **DANGEROUS VOLTAGES PRESENT**: Output voltages up to 150V DC can cause electrical shock or burns
- **STORED ENERGY**: Internal capacitors retain dangerous voltages even when power is disconnected
- **DISCHARGES**: High-energy discharges can cause burns, eye damage, and ignite flammable materials

LEGAL DISCLAIMER
- Users assume all responsibility for safe operation and compliance with local electrical codes
- This equipment is intended for use by qualified professionals only
- Improper use may result in serious injury, death, or property damage
- The manufacturer disclaims all liability for injuries or damages resulting from improper use
**IF YOU ARE NOT QUALIFIED TO WORK WITH HIGH-VOLTAGE EQUIPMENT, DO NOT PROCEED**

## Attribution
The documentation here has been provided without modification from RackRobotics Inc.'s original PowerCoreV3 Repository
[![Github](https://github.com/Rack-Robotics/Powercore-V3)]

## LICENSE 
The WPS by the Electronics Prototyping Laboratory has inherited the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0).

### You are free to:
- **Share** — copy and redistribute the material in any medium or format
- **Adapt** — remix, transform, and build upon the material

### Under the following terms:
- **Attribution** — You must give appropriate credit, provide a link to the license, and indicate if changes were made
- **NonCommercial** — You may not use the material for commercial purposes
- **ShareAlike** — If you remix, transform, or build upon the material, you must distribute your contributions under the same license as the original
- **No additional restrictions** — You may not apply legal terms or technological measures that legally restrict others from doing anything the license permits

### Notices:
You do not have to comply with the license for elements of the material in the public domain or where your use is permitted by an applicable exception or limitation.

No warranties are given. The license may not give you all of the permissions necessary for your intended use. For example, other rights such as publicity, privacy, or moral rights may limit how you use the material.

For the full license text, see: https://creativecommons.org/licenses/by-nc-sa/4.0/

This includes all hardware designs, firmware, documentation, and associated files in this repository. Commercial use requires explicit written permission from the project maintainers. 
