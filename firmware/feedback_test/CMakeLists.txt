cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(feedback_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)

pico_sdk_init()

add_executable(feedback_test
    feedback_test.c
)

target_compile_options(feedback_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_link_libraries(feedback_test
    pico_stdlib
    hardware_gpio
    hardware_pwm
)

pico_enable_stdio_usb(feedback_test 1)
pico_enable_stdio_uart(feedback_test 0)

pico_add_extra_outputs(feedback_test)
