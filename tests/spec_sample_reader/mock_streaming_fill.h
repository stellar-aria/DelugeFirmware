#pragma once

// U2 Task 1: this header pulls in the two C-ABI declarations (`libdeluge/streaming_fill.h`'s
// accessor trio + `storage/audio/stream/chunk_residency.h`'s `deluge_streaming_define_asset`) that
// `mock_streaming_fill.cpp` DEFINES as trivial link-time stand-ins -- real bodies normally supplied
// by the app's `chunk_residency.cpp`/`async_fill.cpp`, which this host test does NOT link. This
// spec only exercises `SampleFrameReader` over an asset with no registered fill-context (see the
// spec file), so none of these ever actually RUN -- but every `deluge_sample_reader_*` call this
// spec makes pulls the whole `deluge_sample_reader`/`deluge_sample_fill` archive members out of
// `deluge_rust_rs_sim`, and THEIR object code references these symbols unconditionally (same
// translation unit as the functions this spec does call), so the link still needs a body for each
// one. Mirrors `deluge_sample_reader`'s own `#[cfg(test)] host_streaming_stubs` module (Rust side)
// and this test suite's sibling `mock_source.h`/`mock_read_source.h` mocks.
#include "libdeluge/streaming_fill.h"
#include "storage/audio/stream/chunk_residency.h"
