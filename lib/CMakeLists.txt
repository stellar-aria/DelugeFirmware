cmake_policy(SET CMP0135 NEW)


# TinyUSB Configuration
set(FAMILY rza1 CACHE STRING "TinyUSB target family")
set(BOARD deluge CACHE STRING "TinyUSB target board")

# Create TinyUSB library
add_library(tinyusb STATIC)

# Add TinyUSB sources (device only for controller mode)
target_sources(tinyusb PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/tusb.c
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/common/tusb_fifo.c
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/device/usbd.c
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/device/usbd_control.c
    # Device class drivers
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/class/cdc/cdc_device.c
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/class/midi/midi_device.c
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/class/audio/audio_device.c
    # RZA1 portable driver
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/portable/renesas/rusb1/dcd_rusb1.c
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/portable/renesas/rusb1/rusb1_common.c
)

# TinyUSB include directories
target_include_directories(tinyusb PUBLIC
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/RZA1/system
    ${CMAKE_SOURCE_DIR}/src/controller
)

# TinyUSB needs these defines
target_compile_definitions(tinyusb PUBLIC
    CFG_TUSB_MCU=OPT_MCU_RZA1X
    CFG_TUSB_OS=OPT_OS_NONE
)
