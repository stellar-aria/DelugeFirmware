# tests/fatfs_stress/CMakeLists.txt
#
# Host-native FatFS reentrancy stress harness. Compiles the REAL src/fatfs/ff.c
# (+ ffunicode.c, diskio.h, ff.h -- unmodified, byte-for-byte) against a
# harness-local ffconf.h that differs from the firmware's src/fatfs/ffconf.h in
# exactly two settings: FF_FS_REENTRANT 0->1 (the change under test) and
# FF_USE_MKFS 0->1 (test setup only). See ffconf.h's header comment for the
# full diff.
#
# Backed by a RAM-disk diskio shim (ram_diskio.cpp) and std::mutex-backed
# FatFS sync stubs (ff_sync.cpp) that replace src/fatfs/ffsystem.c's Win32
# sample code (ffsystem.c is deliberately NOT compiled here). Built with
# -fsanitize=thread so the concurrent workload added in Task 2 gets real
# data-race detection.
#
# --- Why the sources are staged into the build tree (not compiled in place) ---
# ff.h (in src/fatfs/) does `#include "ffconf.h"`. The C/C++ preprocessor's
# quote-include rule resolves that FIRST against the directory containing the
# including file itself (src/fatfs/, which holds the *firmware* ffconf.h) --
# before it ever consults any -I search path, no matter the -I order. So
# simply adding tests/fatfs_stress/ to the include path does NOT shadow
# src/fatfs/ffconf.h; the firmware config would silently win and
# FF_FS_REENTRANT would stay off. To make the harness ffconf.h authoritative,
# byte-identical copies of ff.c/ff.h/diskio.h/ffunicode.c are staged (via a
# build-time `cmake -E copy`, re-run whenever the upstream file changes) into
# a scratch directory alongside our ffconf.h, so ff.h's own directory only
# ever contains the harness config. src/fatfs/* itself is never modified.
set(FATFS_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../src/fatfs)
set(FATFS_STAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fatfs_real)

foreach(f ff.c ff.h diskio.h ffunicode.c)
    add_custom_command(
        OUTPUT ${FATFS_STAGE_DIR}/${f}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${FATFS_STAGE_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${FATFS_SRC_DIR}/${f} ${FATFS_STAGE_DIR}/${f}
        DEPENDS ${FATFS_SRC_DIR}/${f}
        COMMENT "Staging real src/fatfs/${f} for fatfs_stress"
        VERBATIM)
endforeach()

add_custom_command(
    OUTPUT ${FATFS_STAGE_DIR}/ffconf.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FATFS_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/ffconf.h ${FATFS_STAGE_DIR}/ffconf.h
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/ffconf.h
    COMMENT "Staging harness ffconf.h for fatfs_stress"
    VERBATIM)

add_custom_target(fatfs_stress_stage DEPENDS
    ${FATFS_STAGE_DIR}/ff.c ${FATFS_STAGE_DIR}/ff.h ${FATFS_STAGE_DIR}/diskio.h
    ${FATFS_STAGE_DIR}/ffunicode.c ${FATFS_STAGE_DIR}/ffconf.h)

add_executable(fatfs_stress
    ${FATFS_STAGE_DIR}/ff.c
    ${FATFS_STAGE_DIR}/ffunicode.c
    ram_diskio.cpp
    ff_sync.cpp
    main.cpp
)
add_dependencies(fatfs_stress fatfs_stress_stage)

# FATFS_STAGE_DIR first (and only fatfs include dir): it's where the shadowed
# ffconf.h + real ff.h/diskio.h live. tests/fatfs_stress/ itself is added only
# for main.cpp/ram_diskio.cpp/ff_sync.cpp's own headers (none currently).
target_include_directories(fatfs_stress BEFORE PRIVATE
    ${FATFS_STAGE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
)

set_target_properties(fatfs_stress PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

# ThreadSanitizer is 64-bit only, but tests/CMakeLists.txt sets -m32 for every
# target in this directory tree (to match the firmware's 32-bit ABI for the
# other host suites' allocator-conformance testing) -- override back to 64-bit
# for this target alone. -m64 wins over the inherited -m32 because target-level
# compile options are appended after the directory-level ones on the command
# line, and gcc/clang take the last -m32/-m64 seen.
target_compile_options(fatfs_stress PRIVATE -m64 -fsanitize=thread -g -O1)
target_link_options(fatfs_stress PRIVATE -m64 -fsanitize=thread)
target_link_libraries(fatfs_stress PRIVATE pthread)

add_test(NAME fatfs_stress COMMAND fatfs_stress)
set_tests_properties(fatfs_stress PROPERTIES TIMEOUT 120)
