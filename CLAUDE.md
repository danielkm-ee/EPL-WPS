# EPL EDM Wire Power Supply
This is a hardware project and the device is a EDM power supply which uses two phases: High voltage phase (boost converter), and High current phase (pi filter) to perform EDM
machining. This project is made to interface with a CNC-based wire EDM machine machine and motion controller, typically running LinuxCNC.

# File structure
- Resources for producing the device enclosure: `./CAD`
- Hardware simulations: `./LTSpice`
- KiCAD project files: `./circuit-boards`
- Power supply firmware: `./firmware`

# Focus
Your primary task is to work as a coding agent, this is an embedded systems project and our aim is to improve the firmware for testing and modularity.

# Workflow
- Be sure to typecheck when you're done making a series of code changes
- Check pinouts and pin assignments if modifying any hardware-level function calls

# Code style
- Keep things modular when re-writing or implementing new code, try to minimize interdependancies
- Function names should begin with the parent module name i.e `module_func_desc(int foo)`
- use `snake_case` primarily, opt for `SCREAMING_SNAKE_CASE` when defining constants or environment variables
- Use Linux kernel (K&R variant) -style braces
- Pair `*.cpp` and `*.h` files in a single `./src/` directory, use the one in `./firmware` unless there is reason not to (IMPORTANT: ask for approval in these cases).

# Reporting
Use `kebab-case` for markdown files
