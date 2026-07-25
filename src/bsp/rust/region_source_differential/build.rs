//! Compiles `region_fill_differential`'s `cpp/native_finish_shim.cpp` (SR2d-4 Task 6's
//! real-`StreamedChunk` shim) into THIS crate's own test binary, reused verbatim —
//! not copied, not reimplemented. Same file, same real `storage/cluster/cluster.h`
//! layout, same real `deluge_streaming_chunk_payload`/`_set_loaded`/`_convert_state`/
//! `_set_convert_state` accessor bodies (character-for-character identical to
//! `async_fill.cpp`'s own, guarded there by
//! `native_finish_glue.rs::accessor_bodies_match_async_fill_cpp_verbatim`). This crate
//! only needs the chunk-construct/payload-offset half of that shim (`region_fill_diff_
//! chunk_construct`, `_chunk_payload_offset`, `_chunk_backing_size`, `_chunk_header_size`,
//! `_set_cluster_size`) plus the `deluge_streaming_chunk_payload` accessor itself — see
//! `tests/real_chunk_gate.rs`'s module doc for how they're driven.
//!
//! Deliberately does NOT compile `harness_shim.cpp` (the OTHER half of that sibling
//! crate, the fill/convert/stitch orchestration reference) — this gate is about the
//! cursor's payload resolution over a real chunk backing, not the fill orchestration,
//! so pulling in `convert.h`/`stitch.h`/argon/SIMDe here would be dead weight.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // src/bsp/rust/region_source_differential -> repo root is four levels up.
    let repo = manifest
        .join("../../../..")
        .canonicalize()
        .expect("resolve repo root");

    let src = repo.join("src");
    let src_deluge = src.join("deluge");
    let include = repo.join("include");

    // Reused directly from the sibling crate -- NOT copied. Any future edit to the
    // shim (e.g. keeping it in sync with async_fill.cpp) is picked up here too.
    let shim_cpp = manifest
        .join("../region_fill_differential/cpp/native_finish_shim.cpp")
        .canonicalize()
        .expect("resolve region_fill_differential's native_finish_shim.cpp");
    println!("cargo:rerun-if-changed={}", shim_cpp.display());
    println!("cargo:rerun-if-changed=build.rs");

    cc::Build::new()
        .cpp(true)
        .std("c++26")
        .file(&shim_cpp)
        .include(&src) // definitions_cxx.hpp, board_config.h
        .include(&src_deluge) // storage/cluster/cluster.h, memory/general_memory_allocator.h
        .include(&include) // libdeluge/streaming_fill.h
        .define("DELUGE_HOST", None)
        .flag_if_supported("-Wno-unused-parameter")
        .compile("region_source_diff_native_finish_cpp");
}
