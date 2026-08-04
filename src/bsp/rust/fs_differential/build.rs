//! Compiles the real, vendored C FatFS (src/fatfs/ff.c + ffunicode.c) on the
//! host against this crate's harness `ffconf.h`.
//!
//! Why files are staged into OUT_DIR instead of compiled in place (the naive
//! approach of just adding `.include(root)` before `.include(&fatfs)` does
//! NOT work): `ff.c` does `#include "ff.h"` and `ff.h` does
//! `#include "ffconf.h"`, both with the quoted form. The C/C++ preprocessor
//! resolves a quoted include FIRST against the directory containing the
//! *including file itself* -- before it ever consults any `-I`/`.include()`
//! search path, regardless of the order those paths were given. Since ff.c
//! and ff.h live in src/fatfs/ alongside the *firmware* ffconf.h, compiling
//! them in place would silently pull in the firmware ffconf.h no matter how
//! `.include()` is ordered. (Same trap documented in
//! tests/fatfs_stress/CMakeLists.txt for the CMake build of this same
//! vendored source; this is the build.rs equivalent of that staging trick.)
//!
//! The fix: copy ff.c, ff.h, diskio.h, ffunicode.c, and our own ffconf.h into
//! one scratch directory so ff.h's quoted `#include "ffconf.h"` resolves,
//! by the same directory-of-the-including-file rule, to OUR copy.
use std::path::Path;

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let fatfs = root.join("../../../fatfs"); // src/fatfs
    let out_dir = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let stage = out_dir.join("fatfs_stage");
    std::fs::create_dir_all(&stage).expect("create fatfs stage dir");

    for f in ["ff.c", "ff.h", "diskio.h", "ffunicode.c"] {
        let src = fatfs.join(f);
        std::fs::copy(&src, stage.join(f))
            .unwrap_or_else(|e| panic!("stage {}: {e}", src.display()));
        println!("cargo:rerun-if-changed={}", src.display());
    }
    let our_ffconf = root.join("ffconf.h");
    std::fs::copy(&our_ffconf, stage.join("ffconf.h")).expect("stage ffconf.h");
    println!("cargo:rerun-if-changed={}", our_ffconf.display());

    cc::Build::new()
        .file(stage.join("ff.c"))
        .file(stage.join("ffunicode.c"))
        .include(&stage) // ONLY the staged dir: ff.h's quoted ffconf.h must resolve here.
        // Host-only marker src/fatfs/ff.h itself branches on: without it, FIL/FATFS's
        // sector-buffer members pick up an `aligned(32)` attribute (needed on-target
        // for DMA) that a heap allocation with plain Rust `align_of` (1, since our
        // FFI wrapper types are opaque `[u8; N]` byte blobs) cannot satisfy. See
        // src/fatfs/ff.h's FF_CACHE_ALIGN comment.
        .define("DELUGE_HOST", None)
        .flag_if_supported("-Wno-unused-parameter")
        .compile("ffharness");
}
