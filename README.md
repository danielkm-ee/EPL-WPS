# Spark EDM: An EDM power supply for hobby CNCs

A high-efficiency power supply unit designed for Electrical Discharge Machining (EDM) applications. The design is based on the Rack Robotics Powercore V3 and owes much to their engineering efforts.

## EPL Contributions

The EPL has reworked a few aspects of the Rack Robotics Powercore V3 for use in it's CNC-Type Wire EDM. Primarily, we've made use of 2.54mm pin headers in place of the spring-based connectors, or Pogo pins, to simplify construction without the enclosure. Other modifications have been made the the firmware project structure, separating sensor drivers, boost converter, fault handling, and other portions of the project into a modular C++ library. We have also implemented a frequency-coded feedback signal for compatibility with encoder and edge counting inputs available on many CNC control boards.

If you would like to explore or purchase the original PowerCore V3, please vist Rack Robotics at
![The originial PowerCore V3 Repo](https://github.com/Rack-Robotics/Powercore-V3)
and
![Rackrobo.io](https://rackrobo.io/products/powercore-v3)

# Repository Structure

```
├── docs               - documentation files & repository resources
│   ├── img                - images for README.md's
│   ├── LTSpice            - LTSpice Simulations
│   └── schematics         - pdfs of the schematics
├── firmware           - firmware project and library based on the PicoSDK
│   ├── docs               - docs for the firmware
│   ├── feedback_test      - firmware which outputs a frequency sweep for feedback testing
│   ├── src                - source files for the Spark EDM firmware
│   └── tpl0401b_test      - test firmware for testing the boost module potentiometer
├── hardware           - hardware resources for PCBs and the enclosure
│   ├── CAD                - CAD resources for enclosure design & system documentation
│   ├── circuit-boards     - circuit boards for the Spark EDM
│   └── KiCAD-library      - KiCAD library for the Spark EDM
└── tools              - tools for flashing & communicating with the Spark EDM
```

# Building the EDM Supply
The Spark EDM is not available for purchase through the EPL or any other means, but access to the manufacturing resources are provided for those with experience ordering and building up PCBs through the likes of JLCPCB, Oshpark, or PCBWay.

# LICENSE 
This project is based on Powercore V3 by Rack Robotics, Inc., used under CC BY-NC-SA 4.0.

Original copyright: © 2025 Rack Robotics, Inc.

Modifications and additions by Portland State University Electronics Prototyping Lab (PSU EPL) are also licensed under CC BY-NC-SA 4.0.

## Changes from Original
* Firmare:
    * Migration from Arduino to pure PicoSDK
    * new project structure and firmware documetation
    * frequency-coded feedback signal
    * testing firmware for the tpl10401b
    * tools for flashing and communicating with the device
    * extended command line interface with commands for setting the gap voltage and machining frequency
    * conventionalized responses from the serial interface for debugging
    * modularized library for the various functions of the power supply for extendability
    * fault handler and context management utilities
* Hardware:
    * Replacement of the RackRobotics and PowercoreV3 logo in compliance with copyright restrictions
    * Use of 2.54mm headers in place of spring-loaded connectors for ease of assembly
    * Removal of thermal reliefs on high-current capacitors in the pi-filter and boost converter module
    * Layout changes with larger trace widths for high current lines
    * Adaptations for 2.54mm connectors
    * Edge cutout for Pi Pico micro-USB and testpoints
    * Updated component values for missing JCL & digikey part numbers
* Documentation
    * Updated schematics reflecting changes in PCB implementation
    * Firmware documentation reflecting new library, developers manual, and coding conventions for maintenence and new feature introduction
    * Developer's notes from capstone members at Portland State University

## You are free to:
* Share — copy and redistribute the material in any medium or format
* Adapt — remix, transform, and build upon the material

### Under the following terms:
* **Attribution** — You must give appropriate credit, provide a link to the license, and indicate if changes were made
* **NonCommercial** — You may not use the material for commercial purposes
* **ShareAlike** — If you remix, transform, or build upon the material, you must distribute your contributions under the same license

For the full license text, see: https://creativecommons.org/licenses/by-nc-sa/4.0/

### Notices:
You do not have to comply with the license for elements of the material in the public domain or where your use is permitted by an applicable exception or limitation.

No warranties are given. The license may not give you all of the permissions necessary for your intended use. For example, other rights such as publicity, privacy, or moral rights may limit how you use the material.

For the full license text, see: https://creativecommons.org/licenses/by-nc-sa/4.0/

This includes all hardware designs, firmware, documentation, and associated files in this repository. Commercial use requires explicit written permission from the project maintainers. 
