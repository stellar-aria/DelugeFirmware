# tests/spec_storage/CMakeLists.txt
#
# deluge::storage specs. Links the seam(s) under src/deluge/storage/ against
# mock_worker.cpp, a host-only stand-in for include/libdeluge/worker.h's
# deluge_worker_run (runs the op inline -- see mock_worker.cpp), NOT the real
# scheduler/Embassy dispatch (that's the firmware build's concern).
include(FetchContent)
FetchContent_Declare(CppSpec
  URL https://github.com/toroidal-code/cppspec/archive/refs/heads/main.tar.gz
)
FetchContent_MakeAvailable(CppSpec)

function(create_specs_driver driver_name spec_dir)
  file(GLOB_RECURSE spec_sources RELATIVE ${spec_dir} ${spec_dir}/*_spec.cpp)
  create_test_sourcelist(specs ${driver_name}.cpp ${spec_sources})
  add_executable(${driver_name}
    ${specs}
    mock_worker.cpp
    ../../src/deluge/storage/owner.cpp
  )
  target_link_libraries(${driver_name} PRIVATE c++spec)
  target_include_directories(${driver_name} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ../../include      # libdeluge/worker.h
    ../../src/deluge    # storage/owner.h
  )
  set_target_properties(${driver_name} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED YES
  )
  foreach(spec IN LISTS spec_sources)
    cmake_path(GET spec STEM LAST_ONLY spec_name)
    add_test(NAME ${spec_name} COMMAND ${driver_name} ${spec_name} --verbose)
  endforeach()
endfunction()

create_specs_driver(storage_specs ${CMAKE_CURRENT_LIST_DIR})
