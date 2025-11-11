file(GLOB_RECURSE RZA1_SOURCES CONFIGURE_DEPENDS *.S *.c *.h)

# Exclude Renesas USB drivers (replaced with TinyUSB)
list(FILTER RZA1_SOURCES EXCLUDE REGEX ".*/usb/.*")

# Exclude diskio and SD card support (not needed for USB controller)
list(FILTER RZA1_SOURCES EXCLUDE REGEX ".*/diskio.c")
list(FILTER RZA1_SOURCES EXCLUDE REGEX ".*/sdhi/.*")

add_library(RZA1 STATIC ${RZA1_SOURCES})
target_include_directories(RZA1 PRIVATE ${SHARED_INCLUDE})

target_compile_definitions(RZA1 PRIVATE
    $<$<CONFIG:DEBUG>:ENABLE_TEXT_OUTPUT=1>
)

if(ENABLE_RTT)
  message(STATUS "RTT enabled for RZA1")

  target_link_libraries(RZA1 PRIVATE RTT)

  target_compile_definitions(RZA1 PRIVATE
      $<$<CONFIG:DEBUG>:HAVE_RTT=1>
  )
endif(ENABLE_RTT)
