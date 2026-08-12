#! /usr/bin/env python3
"""Build the firmware: C++ app under clang/ThinLTO, linked by the Rust BSP via lld.

Two steps, same shape as `dbt rust`, different toolchain:

  1. cmake builds the C++ app objects with clang. Under Release these are LLVM
     bitcode, not ELF.
  2. cargo archives them and links the Rust firmware with lld, running LTO
     across the Rust/C++ boundary.

Why this is a separate tree from `dbt rust`'s `build/`: that one is GCC, and the
two are NOT interchangeable. arm-none-eabi GCC mangles int32_t as `long` where
clang mangles it as `int`, so a GCC-built object's references do not resolve
against a clang-built one. Mixing is all-or-nothing across any C++ interface
using the fixed-width integer types, hence `build-clang/` alongside `build/`.

`dbt rust` remains the GCC path and is unchanged. Its C++ objects cannot use
LTO — the Rust link cannot read GCC's slim-LTO objects — so it builds Debug
objects at -O2 (see DELUGE_DEBUG_OPT_LEVEL in the root CMakeLists). This task
has no such restriction, which is most of the point: measured on Release, the
clang image is ~315 KB smaller and leaves 537 KB of SRAM free against 222 KB.

NOTE: this no longer builds the legacy C++-only `deluge` executable, which is
retired (see scripts/cmake/retired_rza1_bsp_guard.cmake). To build it anyway:
  cmake --build build --target deluge -DDELUGE_ALLOW_RETIRED_RZA1_BSP=ON
"""

import argparse
import os
import shutil
import subprocess
from collections.abc import Sequence
from pathlib import Path

import util

# The C++ targets the Rust build.rs archives + links (mirrors its panic message).
CPP_TARGETS = [
    "deluge_app",
    "NE10",
    "eyalroz_printf",
    "deluge_dsp",
    "deluge_scheduler",
    "deluge_foundation",
    "deluge_midi",
]

RUST_BSP_DIR = Path("src/bsp/rust")
BUILD_DIR = "build-clang"
TOOLCHAIN_FILE = "scripts/cmake/CMakeToolchainDelugeClang.cmake"
# Feature set matching clang's, so LLVM will inline across the language boundary.
RUST_TARGET = "./armv7a-deluge-eabihf.json"
MULTILIB = "thumb/v7-a+simd/hard"

BUILD_CONFIGS = {"release": "Release", "debug": "Debug"}


def argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="build",
        description="Build the firmware (clang/ThinLTO C++ + Rust BSP, linked by lld)",
    )
    parser.group = "Building"
    parser.add_argument(
        "-v", "--verbose", help="Verbose cmake + cargo output", action="store_true"
    )
    parser.add_argument(
        "-c",
        "--clean-first",
        help="Clean the C++ app objects before rebuilding",
        action="store_true",
    )
    parser.add_argument(
        "config",
        nargs="?",
        default="release",
        choices=list(BUILD_CONFIGS.keys()) + list(BUILD_CONFIGS.values()),
        help="Build configuration (default: release — where ThinLTO pays off)",
    )
    # Anything unrecognised is forwarded to `cargo build` (e.g. --features).
    return parser


def device_cxx_env(root: Path, config: str) -> dict[str, str]:
    """Env for the pieces of the build that shell out to a C++ compiler themselves.

    sample_convert's build.rs compiles a device shim directly rather than through
    cmake, so it needs the same compiler and flags — a GCC-built shim would not
    link against a clang-built app (see the int32_t mangling note above).
    """
    gcc = root / "toolchain/current/arm-none-eabi-gcc"
    sysroot = gcc / "arm-none-eabi"
    # The GCC version dir moves with toolchain bumps; discover it.
    cxx_inc = sorted((sysroot / "include/c++").glob("*"))
    if not cxx_inc:
        raise SystemExit(f"no libstdc++ headers under {sysroot / 'include/c++'}")

    flags = [
        "--target=armv7a-none-eabihf",
        "-mcpu=cortex-a9",
        # neon-fp16, not neon: -mfpu=neon DISABLES fp16, which rustc leaves on by
        # the cortex-a9 default, and that mismatch blocks cross-language inlining.
        "-mfpu=neon-fp16",
        "-mfloat-abi=hard",
        "-mthumb",
        "-mlittle-endian",
        "-funsafe-math-optimizations",
        "-stdlib=libstdc++",
        f"--sysroot={sysroot}",
        f"--gcc-toolchain={gcc}",
        # bits/c++config.h is per-multilib, and the fallback copy is soft-float.
        f"-isystem{cxx_inc[0]}/arm-none-eabi/{MULTILIB}",
    ]
    # Absolute paths, not bare names: the build scripts check is_file() on these.
    clangxx = shutil.which("clang++")
    if clangxx is None:
        raise SystemExit("clang++ not found on PATH (needed for the device shim)")
    # GNU ar cannot index LLVM bitcode: it would write an archive whose symbol
    # index is empty for every member, and the linker would pull in none of them.
    llvm_ar = shutil.which("llvm-ar")
    if llvm_ar is None:
        raise SystemExit("llvm-ar not found on PATH (GNU ar cannot index bitcode)")

    return {
        "DELUGE_BUILD_DIR": str(root / BUILD_DIR),
        "DELUGE_BUILD_CONFIG": config,
        "DELUGE_DEVICE_CXX": clangxx,
        "DELUGE_DEVICE_CXXFLAGS": " ".join(flags),
        "DELUGE_DEVICE_AR": llvm_ar,
    }


def main(argv: Sequence[str] | None = None) -> int:
    (args, cargo_extra) = argparser().parse_known_args(argv)

    root = util.get_git_root()
    os.chdir(root)
    config = BUILD_CONFIGS.get(args.config, args.config)

    # ── Configure the clang tree if it isn't already ─────────────────────────
    if not os.path.exists(BUILD_DIR):
        configure = [
            "cmake",
            "-S",
            ".",
            "-B",
            BUILD_DIR,
            "-G",
            "Ninja Multi-Config",
            f"-DCMAKE_TOOLCHAIN_FILE={root / TOOLCHAIN_FILE}",
        ]
        result = util.run(configure)
        if result != 0:
            return result

    # ── Step 1: build the C++ app objects (bitcode under Release) ────────────
    cmake_args = [
        "cmake",
        "--build",
        BUILD_DIR,
        "--config",
        config,
        "--target",
        *CPP_TARGETS,
    ]
    if args.verbose:
        cmake_args += ["--verbose"]
    if args.clean_first:
        cmake_args += ["--clean-first"]
    else:
        os.environ["NINJA_STATUS"] = "[%s/%t %p :: %e] "
    result = util.run(cmake_args)
    if result != 0:
        return result

    # ── Step 2: archive the C++ objects + link the Rust firmware with lld ────
    # -Zbuild-std because the target is a JSON spec with no prebuilt core; the
    # spec exists to match clang's target features (see armv7a-deluge-eabihf.json).
    cargo_args = [
        "cargo",
        "build",
        "--target",
        RUST_TARGET,
        "-Zbuild-std=core,alloc",
        "-Zjson-target-spec",
    ]
    if config == "Release":
        cargo_args += ["--release"]
    if args.verbose:
        cargo_args += ["--verbose"]
    cargo_args += cargo_extra

    env = {**os.environ, **device_cxx_env(root, config)}
    return subprocess.run(cargo_args, cwd=RUST_BSP_DIR, env=env, check=False).returncode


if __name__ == "__main__":
    main()
