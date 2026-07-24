//! Build script for `deluge_sample_convert`.
//!
//! Compiles the app's `convert.cpp` + `stitch.cpp` (+ their `audio_format_helpers.cpp` dep + the
//! `extern "C"` shim in cpp/shim.cpp) differently depending on which target this crate is actually
//! being built FOR (`CARGO_CFG_TARGET_OS`, the same check `deluge-bsp-rust`'s own build.rs uses):
//!
//!   1. HOST (`cargo test`/a plain `cargo build` of this crate, or as a dependency of a host/`host_app`
//!      build): x86 via `cc::Build` + SIMDe — LINKED into the test binary so the #[test] FFI round-trips
//!      can prove the objects behave correctly (mirroring tests/spec_audio_stream/*_spec.cpp) — PLUS the
//!      armv7a-NEON compile-and-verify check (no QEMU): the produced objects must be ARM ELF, and
//!      convert's object must contain real NEON codegen (`vcvt.s32.f32`, the FLOAT->Q31 path). The
//!      result is stamped to OUT_DIR/arm_compile_result.txt and asserted by a #[test].
//!   2. DEVICE (`target_os = "none"`, i.e. as a dependency of `deluge-bsp-rust`'s real
//!      `cargo device`/armv7a-none-eabihf build — SR2d-4 Task 5, this crate's first real consumer):
//!      compiles ONLY `cpp/shim.cpp` for armv7a-NEON with the real device flags (no SIMDe, the
//!      toolchain's real `<arm_neon.h>`), archives that one object, and emits the `cargo:rustc-link-lib`
//!      directive so `deluge-bsp-rust`'s final device link picks it up. Deliberately does NOT recompile
//!      `convert.cpp`/`stitch.cpp`/`audio_format_helpers.cpp` for the device — see [`build_device`]'s doc
//!      for why (duplicate-definition avoidance against the app's own already-linked objects).
//!
//! Dep ownership (SR2d gotcha #4): argon + SIMDe are header-only and pinned by tag. This crate OWNS them
//! — `fetch_pinned` git-fetches each at the SAME SHA the CMake FetchContent uses into a gitignored
//! third_party/ cache. It does NOT scavenge a CMake build-*/_deps tree, so a standalone `cargo test`
//! works from a clean checkout (with network) without any CMake build having run first. The DEVICE path
//! only needs argon (no SIMDe — the real target never uses the portable fallback).
//!
//! Recipe (flags/defines/include dirs) mirrors tests/spec_audio_stream/CMakeLists.txt,
//! sim/CMakeLists.txt, and scripts/cmake/CMakeToolchainDeluge.cmake.
use std::path::{Path, PathBuf};
use std::process::Command;

/// `scripts/cmake/CMakeToolchainDeluge.cmake`'s `ARCH_FLAGS` (`-mcpu`/`-mfpu`/`-mfloat-abi`/`-mthumb`/
/// `-mthumb-interwork`/`-mlittle-endian`) plus `-funsafe-math-optimizations` ("required to use NEON
/// instead of VFPv3 for floating point", same file) — verified against that file directly, not just
/// copied from the pre-existing [`arm_compile_check`]. Shared by the HOST-side verify-only compile and
/// the DEVICE-side real compile ([`build_device`]) so the two can never drift apart.
const ARM_ARCH_FLAGS: [&str; 7] = [
    "-mcpu=cortex-a9",
    "-mfpu=neon",
    "-mfloat-abi=hard",
    "-mthumb",
    "-mthumb-interwork",
    "-mlittle-endian",
    "-funsafe-math-optimizations",
];

// Pins — MUST match the CMake FetchContent tags (tests/spec_audio_stream/CMakeLists.txt, sim/CMakeLists.txt).
const ARGON_URL: &str = "https://github.com/stellar-aria/argon";
const ARGON_SHA: &str = "b04f7e82ff33523875aca36167fa5bf7ffaf507b";
const SIMDE_URL: &str = "https://github.com/simd-everywhere/simde";
const SIMDE_SHA: &str = "71fd833d9666141edcd1d3c109a80e228303d8d7"; // == tag v0.8.2

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // src/bsp/rust/sample_convert -> repo root is four levels up.
    let repo = manifest
        .join("../../../..")
        .canonicalize()
        .expect("resolve repo root");
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

    // --- Own the pinned deps: fetch argon (+ SIMDe, host-only) into a gitignored third_party/ cache. ---
    let third_party = manifest.join("third_party");
    let argon_dir = third_party.join("argon");
    fetch_pinned(
        "argon",
        ARGON_URL,
        ARGON_SHA,
        &argon_dir,
        Path::new("include/argon.hpp"),
    );
    let argon_inc = argon_dir.join("include");

    // SR2d-4 Task 5: this crate now has a real consumer (`deluge-bsp-rust`'s native fill task), which
    // links it on the ACTUAL armv7a-none-eabihf device target, not just the x86 host test binary. Same
    // `CARGO_CFG_TARGET_OS` check `deluge-bsp-rust`'s own build.rs uses to distinguish device from host.
    let device = std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("none");
    if device {
        build_device(
            &repo,
            &argon_inc,
            &src,
            &src_deluge,
            &include,
            &out_dir,
            &shim_cpp,
        );
        return;
    }

    // --- HOST-only from here down: SIMDe is only ever needed for the x86 cc::Build below. -------------
    let simde_dir = third_party.join("simde");
    fetch_pinned(
        "simde",
        SIMDE_URL,
        SIMDE_SHA,
        &simde_dir,
        Path::new("simde/arm/neon.h"),
    );
    let simde_root = simde_dir;

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
        .include(&src) // definitions_cxx.hpp (SHARED_INCLUDE)
        .include(&src_deluge) // storage/..., util/...
        .include(&include) // libdeluge/types.h
        // argon + SIMDe (non-native host): argon's bare <arm/neon.h> and the compat <arm_neon.h> shim.
        .include(&argon_inc) // argon.hpp, argon/helpers/size.hpp
        .include(&sim_compat) // <arm_neon.h> -> SIMDe shim
        .include(&simde_root) // compat shim's <simde/arm/neon.h>
        .include(simde_root.join("simde")) // argon's bare <arm/neon.h>
        // Host markers, mirroring sim/CMakeLists.txt.
        .define("DELUGE_HOST", None)
        .define("SIMDE_NO_NATIVE", None) // portable-C NEON fallback = bit-accurate to device NEON
        .flag_if_supported("-Wno-unused-parameter");
    build.compile("deluge_sample_convert_cc");

    // ============================================================================================
    // 2. armv7a-NEON device compile-and-verify via the repo's arm-none-eabi g++ (no QEMU; -c only).
    // ============================================================================================
    let arm_result = arm_compile_check(
        &repo,
        &argon_inc,
        &src,
        &src_deluge,
        &include,
        &out_dir,
        &convert_cpp,
        &[&convert_cpp, &stitch_cpp, &helpers_cpp, &shim_cpp],
    );
    let stamp = out_dir.join("arm_compile_result.txt");
    std::fs::write(&stamp, &arm_result).expect("write arm stamp");
    if !arm_result.starts_with("OK") {
        println!("cargo:warning=armv7a-NEON compile/verify FAILED (see test output / stamp)");
    }
}

/// Git-fetch `url` at exactly `sha` into `dest` (shallow), if the checkout marker isn't already present
/// at the pinned SHA. Owns the dep reproducibly without depending on a CMake `_deps` tree.
fn fetch_pinned(name: &str, url: &str, sha: &str, dest: &Path, marker_rel: &Path) {
    let marker = dest.join(marker_rel);
    let sha_stamp = dest.join(".pinned-sha");
    let up_to_date = marker.is_file()
        && std::fs::read_to_string(&sha_stamp)
            .map(|s| s.trim() == sha)
            .unwrap_or(false);
    if up_to_date {
        return;
    }

    // Stale/partial checkout — start clean.
    let _ = std::fs::remove_dir_all(dest);
    std::fs::create_dir_all(dest).unwrap_or_else(|e| panic!("mkdir {}: {e}", dest.display()));

    let git = |args: &[&str]| -> bool {
        Command::new("git")
            .current_dir(dest)
            .args(args)
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    };
    let ok = git(&["init", "-q"])
        && git(&["remote", "add", "origin", url])
        && git(&["fetch", "-q", "--depth", "1", "origin", sha])
        && git(&[
            "-c",
            "advice.detachedHead=false",
            "checkout",
            "-q",
            "FETCH_HEAD",
        ]);
    if !ok || !marker.is_file() {
        panic!(
            "deluge_sample_convert: failed to fetch pinned {name} ({url} @ {sha}) into {}. \
             This crate owns its deps via a shallow git fetch (needs network on first build). \
             Marker {} missing after checkout.",
            dest.display(),
            marker.display()
        );
    }
    std::fs::write(&sha_stamp, sha).ok();
}

/// Compile each TU for armv7a-NEON with the real device flags (-c only, real <arm_neon.h>), then verify
/// the produced objects are ARM ELF and that `convert`'s object carries real NEON codegen. Returns "OK\n
/// <details>" or "FAIL\n<log>".
#[allow(clippy::too_many_arguments)]
fn arm_compile_check(
    repo: &Path,
    argon_inc: &Path,
    src: &Path,
    src_deluge: &Path,
    include: &Path,
    out_dir: &Path,
    convert_cpp: &Path,
    files: &[&PathBuf],
) -> String {
    // `toolchain/current` symlinks to the active toolchain version's host dir (mirrors
    // `deluge-bsp-rust`'s own build.rs), so this survives version bumps.
    let bin = repo.join("toolchain/current/arm-none-eabi-gcc/bin");
    let gxx = bin.join("arm-none-eabi-g++");
    let objdump = bin.join("arm-none-eabi-objdump");
    if !gxx.is_file() {
        return format!("FAIL\narm-none-eabi-g++ not found at {}", gxx.display());
    }

    let mut log = String::new();
    let mut convert_obj: Option<PathBuf> = None;
    for f in files {
        let obj = out_dir.join(format!(
            "{}.arm.o",
            f.file_name().unwrap().to_string_lossy()
        ));
        let out = Command::new(&gxx)
            .args(["-std=c++26", "-c"])
            .args(ARM_ARCH_FLAGS)
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
            Ok(o) if o.status.success() => {
                if *f == convert_cpp {
                    convert_obj = Some(obj);
                }
            }
            Ok(o) => log.push_str(&format!(
                "--- {} ---\n{}\n",
                f.display(),
                String::from_utf8_lossy(&o.stderr)
            )),
            Err(e) => log.push_str(&format!("--- {} --- spawn error: {e}\n", f.display())),
        }
    }
    if !log.is_empty() {
        return format!("FAIL\n{log}");
    }

    // Verify ARM ELF + real NEON codegen in convert's object (the FLOAT->Q31 vcvt.s32.f32 path).
    let convert_obj = convert_obj.expect("convert.cpp arm object produced");
    if !objdump.is_file() {
        return "OK\narm objects produced (objdump absent, NEON not disassembly-verified)\n"
            .to_string();
    }
    let arch = Command::new(&objdump)
        .args(["-f"])
        .arg(&convert_obj)
        .output();
    let arch_str = arch
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default();
    let is_arm_elf =
        arch_str.contains("elf32-littlearm") || arch_str.to_lowercase().contains("arm");
    if !is_arm_elf {
        return format!("FAIL\nconvert.cpp object is not ARM ELF:\n{arch_str}");
    }
    let dis = Command::new(&objdump)
        .args(["-d"])
        .arg(&convert_obj)
        .output();
    let dis_str = dis
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default();
    let has_neon = dis_str.contains("vcvt.s32.f32");
    if !has_neon {
        return "FAIL\nARM ELF produced but no vcvt.s32.f32 NEON codegen found in convert.cpp object"
            .to_string();
    }
    format!(
        "OK\nARM ELF + real NEON (vcvt.s32.f32) confirmed in convert.cpp object\n{}",
        arch_str
            .lines()
            .find(|l| l.contains("file format"))
            .unwrap_or("")
            .trim()
    )
}

/// SR2d-4 Task 5: the REAL armv7a-NEON build for the device target — this crate's first actual
/// consumer (`deluge-bsp-rust`'s native fill task) links it into the real firmware image, not just a
/// verify-only compile. Compiles ONLY `cpp/shim.cpp` (real device NEON flags, no SIMDe, the toolchain's
/// real `<arm_neon.h>` — same recipe as [`arm_compile_check`]'s per-TU compile, just for one file and
/// actually archived+linked this time), then archives that one object and emits the `cargo:rustc-link-*`
/// directives `deluge-bsp-rust`'s final device link needs.
///
/// Deliberately does **NOT** recompile `convert.cpp`/`stitch.cpp`/`audio_format_helpers.cpp` for the
/// device, unlike the host branch's `cc::Build` above. Those three TUs are already part of the app's own
/// CMake `deluge_app` target (`storage/audio/stream/convert.cpp`/`stitch.cpp`,
/// `util/audio_format_helpers.cpp` are in the shared `deluge_SOURCES` glob — the SAME TUs the legacy C++
/// `finish_fill` sync path already calls on device), and `deluge-bsp-rust`'s own build.rs already
/// archives+links that WHOLE object closure (`libdeluge_app_objs.a`) into the same final device binary.
/// Compiling them a SECOND time here, into a SECOND archive, would risk a duplicate-definition link
/// error for their non-template exported symbols (`convert_word`, `stitch_boundaries`,
/// `q31_from_float`/`swapEndianness*`) — instead, `shim.cpp`'s own object is compiled with those
/// references left UNDEFINED, and the final device link resolves them against the SINGLE, already-
/// compiled definitions already present in `libdeluge_app_objs.a` (ordinary static-archive symbol
/// resolution — no different from any other cross-TU call within the app itself).
/// `convert_cluster_data`/`convert_word_range` (templates, defined inline in `convert.h`) are unaffected
/// either way: each TU that calls them gets its own instantiation compiled directly into its own object
/// file (`shim.cpp`'s own no-op-Yield instantiation is a distinct mangled symbol from the app's own
/// real-Yield instantiation, so there is no clash there regardless of this choice).
#[allow(clippy::too_many_arguments)]
fn build_device(
    repo: &Path,
    argon_inc: &Path,
    src: &Path,
    src_deluge: &Path,
    include: &Path,
    out_dir: &Path,
    shim_cpp: &Path,
) {
    let bin = repo.join("toolchain/current/arm-none-eabi-gcc/bin");
    let gxx = bin.join("arm-none-eabi-g++");
    let ar = bin.join("arm-none-eabi-ar");
    assert!(
        gxx.is_file(),
        "arm-none-eabi-g++ not found at {}",
        gxx.display()
    );
    assert!(
        ar.is_file(),
        "arm-none-eabi-ar not found at {}",
        ar.display()
    );

    let obj = out_dir.join("shim.cpp.o");
    let status = Command::new(&gxx)
        .args(["-std=c++26", "-c"])
        .args(ARM_ARCH_FLAGS)
        // Native <arm_neon.h> — no SIMDe, no compat shim on the real target (matches
        // `arm_compile_check`'s device recipe).
        .arg(format!("-I{}", argon_inc.display()))
        .arg(format!("-I{}", src.display()))
        .arg(format!("-I{}", src_deluge.display()))
        .arg(format!("-I{}", include.display()))
        .arg(shim_cpp)
        .arg("-o")
        .arg(&obj)
        .status()
        .expect("run arm-none-eabi-g++ (device shim.cpp compile)");
    assert!(
        status.success(),
        "device build of cpp/shim.cpp failed (see g++ output above)"
    );

    let archive = out_dir.join("libdeluge_sample_convert_cc.a");
    let _ = std::fs::remove_file(&archive);
    let status = Command::new(&ar)
        .arg("crs")
        .arg(&archive)
        .arg(&obj)
        .status()
        .expect("run arm-none-eabi-ar");
    assert!(status.success(), "archiving device shim.cpp object failed");

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=deluge_sample_convert_cc");
}
