# tests/spec_audio_stream/CMakeLists.txt
#
# deluge::audio::stream ReadSource specs. Exercises the seam via an in-memory MockReadSource
# (header-only) — no card, no FatFS, no resource manager.
include(FetchContent)
FetchContent_Declare(CppSpec
  URL https://github.com/toroidal-code/cppspec/archive/refs/heads/main.tar.gz
)
FetchContent_MakeAvailable(CppSpec)

function(create_specs_driver driver_name spec_dir)
  file(GLOB_RECURSE spec_sources RELATIVE ${spec_dir} ${spec_dir}/*_spec.cpp)
  create_test_sourcelist(specs ${driver_name}.cpp ${spec_sources})
  add_executable(${driver_name} ${specs}
    # convert_spec.cpp exercises the real convert_word implementation (not a mock), so its
    # translation unit must be linked in. stitch_spec.cpp exercises the real stitch_boundaries
    # (which itself calls convert_word), so stitch.cpp must be linked too.
    ../../src/deluge/storage/audio/stream/convert.cpp
    ../../src/deluge/storage/audio/stream/stitch.cpp
    ../../src/deluge/util/audio_format_helpers.cpp
  )
  target_link_libraries(${driver_name} PRIVATE c++spec)
  target_include_directories(${driver_name} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ../../include      # libdeluge/types.h
    ../../src         # definitions_cxx.hpp
    ../../src/deluge   # storage/audio/stream/read_source.h, io/stream.hpp
  )
  set_target_properties(${driver_name} PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES)
  foreach(spec IN LISTS spec_sources)
    cmake_path(GET spec STEM LAST_ONLY spec_name)
    add_test(NAME ${spec_name} COMMAND ${driver_name} ${spec_name} --verbose)
  endforeach()
endfunction()

create_specs_driver(audio_stream_specs ${CMAKE_CURRENT_LIST_DIR})
