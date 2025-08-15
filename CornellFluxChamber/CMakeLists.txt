# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# initalize pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NEVER EDIT THE NEXT LINES for Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.0.0)
set(toolchainVersion 13_2_Rel1)
set(picotoolVersion 2.0.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# We also need PICO EXTRAS
set(PICO_EXTRAS_FETCH_FROM_GIT on)
include(pico_extras_import.cmake)

project(CornellFluxChamber C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()
add_subdirectory(FatFs_SPI)
add_subdirectory(SCD30_I2C)

# Add executable. Default name is the project name, version 0.1
add_executable(CornellFluxChamber 
    # terminal_interface.c
    flux_chamber.c
)

# Add the standard library,  FatFS/SPI, and SCD30/I2C to the build
target_link_libraries(CornellFluxChamber 
    pico_bootsel_via_double_reset 
    pico_stdlib
    pico_stdio
    pico_stdio_uart
    FatFs_SPI
    SCD30_I2C
    pico_multicore
    pico_unique_id
    hardware_adc
    hardware_rtc
    hardware_sleep
)

pico_set_program_name(CornellFluxChamber "CornellFluxChamber")
pico_set_program_version(CornellFluxChamber "0.1")

# Choose source and destination for standard input and output:
#   See 4.1. Serial input and output on Raspberry Pi Pico in Getting started with Raspberry Pi Pico (https://datasheets.raspberrypi.org/pico/getting-started-with-pico.pdf)
#   and 2.7.1. Standard Input/Output (stdio) Support in Raspberry Pi Pico C/C++ SDK (https://datasheets.raspberrypi.org/pico/raspberry-pi-pico-c-sdk.pdf):
pico_enable_stdio_uart(CornellFluxChamber 1)
pico_enable_stdio_usb(CornellFluxChamber 0)

pico_add_extra_outputs(CornellFluxChamber)

add_compile_options(-Ofast)