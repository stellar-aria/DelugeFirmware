# Controller application CMakeLists.txt

# Controller source files
set(CONTROLLER_SOURCES
    controller.cpp
    hardware_events.cpp
    usb_serial.cpp
    display_control.cpp
    usb_descriptors.c
    usb_audio.c
    usb_midi.c
    oled_init.c
    oled_stubs.c
    # UART driver for PIC32 and MIDI communication
    ${CMAKE_SOURCE_DIR}/src/drivers/uart/uart.c
    ${CMAKE_SOURCE_DIR}/src/drivers/dmac/dmac.c
    # SSI driver for audio I/O
    ${CMAKE_SOURCE_DIR}/src/drivers/ssi/ssi.c
    # SPI driver for OLED/CV
    ${CMAKE_SOURCE_DIR}/src/drivers/rspi/rspi.c
    # OLED driver for display (provides SPI transfer queue variables)
    ${CMAKE_SOURCE_DIR}/src/drivers/oled/oled.c
    # Cache flush for DMA operations
    ${CMAKE_SOURCE_DIR}/src/drivers/cache/invalidate.S
)

# Add controller library
add_library(controller OBJECT ${CONTROLLER_SOURCES})

# Set C++23 standard for controller
set_target_properties(controller PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

# Include directories for controller
target_include_directories(controller PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/deluge  # For util/cfunctions.h used by uart driver
    ${CMAKE_SOURCE_DIR}/src/RZA1/system
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src
    ${CMAKE_SOURCE_DIR}/lib/tinyusb/src/device
)

# Link against dependencies
target_link_libraries(controller PUBLIC
    RZA1
    RTT
    eyalroz_printf
    tinyusb
)
