//! Compiles the standalone C++ region-port slice — the SAME translation units
//! the CppSpec unit test (`tests/spec_audio_stream/`) links — straight into
//! this crate's Rust test binary via `cc::Build`, so the SR2c differential can
//! drive the real C++ backing over FFI.
//!
//! The three C++ TUs, mirroring `tests/spec_audio_stream/CMakeLists.txt`'s
//! `sample_source` slice:
//!   * `src/deluge/storage/audio/stream/sample_source.cpp` — the region port
//!     itself (the `extern "C"` `deluge_sample_source_*` ABI).
//!   * `tests/spec_audio_stream/sample_source_test_support.cpp` — the test-only
//!     `deluge::cluster::add_lease`/`release_lease` fakes, the `Cluster::size`
//!     statics, and the throwing `freezeWithError`.
//!   * `cpp/harness_shim.cpp` — this crate's own `extern "C"` shim over the fake
//!     `SampleStream` (a C++ type Rust cannot construct directly) and the
//!     C++-mangled lease-tracking helpers.
//!
//! Include roots mirror the CMake driver's `target_include_directories` EXACTLY
//! (order matters: `fake_include` must precede `src/deluge` so the fake
//! `storage/audio/stream/sample_stream.h` shadows the real header):
//!   * `tests/spec_audio_stream/fake_include` — the fake `SampleStream`.
//!   * `tests/spec_audio_stream`              — `sample_source_test_support.h`.
//!   * `include`                              — `libdeluge/{types,sample_source}.h`.
//!   * `src`                                  — `definitions_cxx.hpp`.
//!   * `src/deluge`                           — `storage/...`, `foundation/...`.
//!
//! Unlike the SR2b spike, this slice needs NO argon / NO SIMDe: `sample_source.cpp`
//! includes none of the vectorized convert/stitch headers, so there is no
//! `_deps/` scavenging and no arch split. Host-only, x86-64 (the objects link
//! into cargo's 64-bit test binary; the port's pointer-cast lease tokens and
//! lease accounting are bitness-independent — this harness is not a golden
//! oracle, so the SR2b `-m32` deviation note does not bite here).
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // src/bsp/rust/region_differential -> repo root is four levels up.
    let repo = manifest.join("../../../..").canonicalize().expect("resolve repo root");

    let src = repo.join("src");
    let src_deluge = src.join("deluge");
    let include = repo.join("include");
    let spec = repo.join("tests/spec_audio_stream");
    let fake_include = spec.join("fake_include");

    let sample_source_cpp = src_deluge.join("storage/audio/stream/sample_source.cpp");
    let test_support_cpp = spec.join("sample_source_test_support.cpp");
    let shim_cpp = manifest.join("cpp/harness_shim.cpp");

    for f in [&sample_source_cpp, &test_support_cpp, &shim_cpp] {
        println!("cargo:rerun-if-changed={}", f.display());
    }
    println!("cargo:rerun-if-changed=build.rs");

    cc::Build::new()
        .cpp(true)
        .std("c++26")
        .file(&sample_source_cpp)
        .file(&test_support_cpp)
        .file(&shim_cpp)
        // fake_include FIRST — shadows storage/audio/stream/sample_stream.h.
        .include(&fake_include)
        .include(&spec) // sample_source_test_support.h
        .include(&include) // libdeluge/*.h
        .include(&src) // definitions_cxx.hpp
        .include(&src_deluge) // storage/..., foundation/...
        // Host marker the app headers branch on (matches fs_differential's
        // build.rs + the SR2b spike): drops DMA-alignment attributes that a
        // host build can't satisfy.
        .define("DELUGE_HOST", None)
        .flag_if_supported("-Wno-unused-parameter")
        .compile("region_port_cpp");
}
