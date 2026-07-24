//! SPIKE (SR2b) — THROWAWAY de-risk build script, superseded by SR2d.
//!
//! Compiles the app's `convert.cpp` + `stitch.cpp` (+ their `audio_format_helpers.cpp` dep + a thin
//! `extern "C"` shim) two ways, to answer SR2b's question: can a Rust `cc::Build` compile these
//! argon-SIMD translation units at C++26 for BOTH targets?
//!
//!   1. x86 host, via `cc::Build` + SIMDe — LINKED into this crate so a `#[test]` FFI round-trip can
//!      prove the objects behave correctly (the priority).
//!   2. armv7a-NEON device, via the repo's arm-none-eabi g++ with the real device NEON flags — a
//!      COMPILE-check (no QEMU): does argon compile against the toolchain's real <arm_neon.h>? The
//!      result is stamped to OUT_DIR/arm_compile_result.txt and asserted by a `#[test]`, so an arm
//!      failure surfaces as a test failure WITHOUT blocking the x86 correctness test.
//!
//! Recipe (flags/defines/include dirs/deps) is lifted from tests/spec_audio_stream/CMakeLists.txt,
//! sim/CMakeLists.txt, and scripts/cmake/CMakeToolchainDeluge.cmake. See the spike report for details.
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // src/bsp/rust/argon_cc_spike -> repo root is four levels up.
    let repo = manifest.join("../../../..").canonicalize().expect("resolve repo root");
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    let src = repo.join("src");
    let src_deluge = src.join("deluge");
    let include = repo.join("include");
    let sim_compat = repo.join("sim/compat");
    let stream = src_deluge.join("storage/audio/stream");

    let convert_cpp = stream.join("convert.cpp");
    let stitch_cpp = stream.join("stitch.cpp");
    let helpers_cpp = src_deluge.join("util/audio_format_helpers.cpp");
    let shim_cpp = manifest.join("cpp/shim.cpp");
    for f in [&convert_cpp, &stitch_cpp, &helpers_cpp, &shim_cpp] {
        println!("cargo:rerun-if-changed={}", f.display());
    }
    println!("cargo:rerun-if-changed=build.rs");

    // --- Locate the FetchContent'd argon + SIMDe deps in whatever CMake build tree already populated
    //     them. Argon is header-only; SIMDe is header-only. Both are pinned by the CMake builds. ------
    let (argon_inc, simde_root) = find_deps(&repo);

    // ============================================================================================
    // 1. x86 host build via cc::Build + SIMDe (the runnable, linked-in path).
    // ============================================================================================
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++26")
        .file(&convert_cpp)
        .file(&stitch_cpp)
        .file(&helpers_cpp)
        .file(&shim_cpp)
        // App include roots (mirror tests/spec_audio_stream/CMakeLists.txt + sim HOST_INCLUDE_DIRS).
        .include(&src)         // definitions_cxx.hpp (SHARED_INCLUDE)
        .include(&src_deluge)  // storage/..., util/...
        .include(&include)     // libdeluge/types.h
        // argon + SIMDe (non-native host): argon's bare <arm/neon.h> and the compat <arm_neon.h> shim.
        .include(&argon_inc)               // argon.hpp, argon/helpers/size.hpp
        .include(&sim_compat)              // <arm_neon.h> -> SIMDe shim
        .include(&simde_root)              // compat shim's <simde/arm/neon.h>
        .include(simde_root.join("simde")) // argon's bare <arm/neon.h>
        // Host markers, mirroring sim/CMakeLists.txt.
        .define("DELUGE_HOST", None)
        .define("SIMDE_NO_NATIVE", None) // portable-C NEON fallback = bit-accurate to device NEON
        .flag_if_supported("-Wno-unused-parameter");
    build.compile("argon_cc_spike_x86");

    // ============================================================================================
    // 2. armv7a-NEON device compile-check via the repo's arm-none-eabi g++ (no QEMU; -c only).
    // ============================================================================================
    let arm_result = arm_compile_check(&repo, &argon_inc, &src, &src_deluge, &include, &out_dir,
                                       &[&convert_cpp, &stitch_cpp, &helpers_cpp, &shim_cpp]);
    let stamp = out_dir.join("arm_compile_result.txt");
    std::fs::write(&stamp, &arm_result).expect("write arm stamp");
    if !arm_result.starts_with("OK") {
        println!("cargo:warning=SR2b armv7a-NEON compile-check FAILED (see test output / stamp)");
    }
}

/// Find an argon include dir and a SIMDe source root among the repo's CMake build trees.
fn find_deps(repo: &Path) -> (PathBuf, PathBuf) {
    // Prefer trees known to FetchContent all of argon+simde (tests + host-sim variants).
    let candidates = [
        "build-tests", "build-sim-cpp", "build-sim", "build-sim-asan", "build-sim-clang",
        "build-embassy-hostapp", "build-embassy-hostapp-tsan",
    ];
    let mut argon: Option<PathBuf> = None;
    let mut simde: Option<PathBuf> = None;
    for c in candidates {
        let deps = repo.join(c).join("_deps");
        let a = deps.join("argon-src/include");
        let s = deps.join("simde-src");
        if argon.is_none() && a.join("argon.hpp").is_file() {
            argon = Some(a);
        }
        if simde.is_none() && s.join("simde/arm/neon.h").is_file() {
            simde = Some(s);
        }
    }
    let argon = argon.unwrap_or_else(|| panic!(
        "SR2b: could not find argon-src/include/argon.hpp under any build-*/. Populate a CMake build \
         first (e.g. `cmake --preset tests` or `./dbt sim`) so FetchContent fetches argon + simde."));
    let simde = simde.unwrap_or_else(|| panic!(
        "SR2b: could not find simde-src/simde/arm/neon.h under any build-*/. Populate a CMake build \
         first so FetchContent fetches simde."));
    (argon, simde)
}

/// Compile each TU for armv7a-NEON with the real device flags (-c only). Returns "OK" or "FAIL\n<log>".
fn arm_compile_check(repo: &Path, argon_inc: &Path, src: &Path, src_deluge: &Path, include: &Path,
                     out_dir: &Path, files: &[&PathBuf]) -> String {
    let gxx = repo.join("toolchain/v25/linux-x86_64/arm-none-eabi-gcc/bin/arm-none-eabi-g++");
    if !gxx.is_file() {
        return format!("FAIL\narm-none-eabi-g++ not found at {}", gxx.display());
    }
    let mut log = String::new();
    for f in files {
        let obj = out_dir.join(format!("{}.arm.o", f.file_name().unwrap().to_string_lossy()));
        let out = Command::new(&gxx)
            .args(["-std=c++26", "-c"])
            // Device NEON arch flags (scripts/cmake/CMakeToolchainDeluge.cmake ARCH_FLAGS).
            .args(["-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=hard", "-mthumb", "-mthumb-interwork",
                   "-mlittle-endian", "-funsafe-math-optimizations"])
            // Native <arm_neon.h> — no SIMDe, no compat shim on the real target.
            .arg(format!("-I{}", argon_inc.display()))
            .arg(format!("-I{}", src.display()))
            .arg(format!("-I{}", src_deluge.display()))
            .arg(format!("-I{}", include.display()))
            .arg(f.as_path())
            .arg("-o")
            .arg(&obj)
            .output();
        match out {
            Ok(o) if o.status.success() => {}
            Ok(o) => {
                log.push_str(&format!("--- {} ---\n{}\n", f.display(), String::from_utf8_lossy(&o.stderr)));
            }
            Err(e) => log.push_str(&format!("--- {} --- spawn error: {e}\n", f.display())),
        }
    }
    if log.is_empty() { "OK\n".to_string() } else { format!("FAIL\n{log}") }
}
