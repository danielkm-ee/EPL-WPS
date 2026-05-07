cmake_minimum_required(VERSION 3.13)

# Set PICO_SDK_PATH in your environment or pass -DPICO_SDK_PATH=... to cmake.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(epl_wps C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)

pico_sdk_init()

add_executable(epl_wps
    main.c
    src/ctx.c
    src/sensors.c
    src/pmm.c
    src/boost.c
    src/output.c
    src/fault.c
    src/cmd.c
)

target_include_directories(epl_wps PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}      # config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # types.h and all module headers
)

target_compile_options(epl_wps PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_link_libraries(epl_wps
    pico_stdlib       # stdio_usb, sleep_ms, get_absolute_time, to_ms_since_boot
    hardware_adc      # adc_init, adc_read, adc_select_input, adc_gpio_init
    hardware_pwm      # pwm_set_*, pwm_gpio_to_slice_num, pwm_gpio_to_channel
    hardware_gpio     # gpio_init, gpio_set_irq_enabled_with_callback
    hardware_irq      # irq_set_enabled, IO_IRQ_BANK0
    hardware_i2c      # i2c_init, i2c_write_blocking_until
    hardware_watchdog # watchdog_reboot
)

# USB CDC replaces Arduino Serial; disable UART stdio.
pico_enable_stdio_usb(epl_wps 1)
pico_enable_stdio_uart(epl_wps 0)

# Generate .uf2 for drag-drop flashing and .elf/.bin for openocd/picotool.
pico_add_extra_outputs(epl_wps)
