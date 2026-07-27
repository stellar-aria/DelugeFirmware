//! Compiles `cpp/harness_shim.cpp` (U1 Task 5's real-chunk harness + the current-read-path oracle)
//! into this crate's Rust test binary via `cc::Build`. Host-only, x86-64: this crate is a test
//! harness, never linked into the firmware, so (unlike `sample_convert`'s build.rs) there is no
//! device path and no argon/SIMDe fetch.
//!
//! Mirrors the SECOND `cc::Build` in `region_fill_differential/build.rs` (its
//! `native_finish_shim.cpp` block): this shim, like that one, only needs the real, unmodified
//! `storage/cluster/cluster.h` — `StreamedChunk`'s real, compiler-computed layout, `payload()`,
//! `payload_with_trailing_slack()`, and (new here) `frame_read_origin`, the current read path's
//! oracle function this crate's differential drives directly. No convert.h/stitch.h, so no
//! argon/SIMDe dependency at all — this crate's `Cargo.toml` doc explains why `deluge_sample_fill`'s
//! own (transitively pulled) `deluge_sample_convert` dependency already supplies THOSE object files
//! to the final link, without this build script recompiling them a second time.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // src/bsp/rust/region_read_differential -> repo root is four levels up.
    let repo = manifest
        .join("../../../..")
        .canonicalize()
        .expect("resolve repo root");

    let src = repo.join("src");
    let src_deluge = src.join("deluge");
    let include = repo.join("include");

    let shim_cpp = manifest.join("cpp/harness_shim.cpp");
    println!("cargo:rerun-if-changed={}", shim_cpp.display());
    println!("cargo:rerun-if-changed=build.rs");

    cc::Build::new()
        .cpp(true)
        .std("c++26")
        .file(&shim_cpp)
        // App include roots (mirror region_fill_differential's own native_finish_shim.cpp recipe).
        .include(&src) // definitions_cxx.hpp, board_config.h
        .include(&src_deluge) // storage/cluster/cluster.h, memory/general_memory_allocator.h
        .include(&include) // libdeluge/streaming_fill.h
        .define("DELUGE_HOST", None)
        .flag_if_supported("-Wno-unused-parameter")
        .compile("region_read_diff_cpp");
}
