# Loud build-time guard for the RETIRED legacy C/C++ RZA1 firmware BSP.
#
# Invoked as a PRE_LINK step of the `deluge` executable (see src/CMakeLists.txt),
# so it fires only when the RZA1 firmware image is actually being linked — NOT for
# the Rust-BSP `deluge_app` staticlib build, the host-sim, or the unit tests.
#
# Context: as of R1 (the efatfs read-path migration) the streaming sample-read
# path is Rust embedded-fatfs, which lives ONLY in the Rust BSP (src/bsp/rust).
# The RZA1 firmware links the weak `deluge_efatfs_open` no-op, so on this build
# `open_read_stream()` fails and every streamed sample fails to load at runtime.
# The image links cleanly but is non-functional — exactly the "silently broken
# green build" this guard exists to stop from shipping.
message(FATAL_ERROR
    "\n"
    "==============================================================================\n"
    "  The legacy C/C++ RZA1 firmware BSP is RETIRED (as of R1, efatfs read-path\n"
    "  migration). Its streaming sample-read path is NON-FUNCTIONAL:\n"
    "  embedded-fatfs (efatfs) lives only in the Rust BSP (src/bsp/rust), so this\n"
    "  RZA1 image resolves the weak 'deluge_efatfs_open' no-op and EVERY streamed\n"
    "  sample fails to load at runtime. It links cleanly but cannot load samples.\n"
    "\n"
    "  Build the Rust BSP instead (src/bsp/rust).\n"
    "\n"
    "  To build this retired RZA1 firmware anyway (known-broken sample loading,\n"
    "  e.g. for non-streaming bring-up or bisecting), re-configure with:\n"
    "      -DDELUGE_ALLOW_RETIRED_RZA1_BSP=ON\n"
    "\n"
    "  See the 'legacy-bsp-retirement-committed' decision / the rustfs migration\n"
    "  roadmap for why.\n"
    "==============================================================================\n")
