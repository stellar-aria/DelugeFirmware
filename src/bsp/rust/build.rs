use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    // DelugeFirmware repo root (crate is at <root>/src/bsp/rust).
    let repo_root = manifest_dir.join("../../..").canonicalize().unwrap();
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    // Host (platform-std) build: most of the below is device-only (the rza1l
    // linker script, the arm-eabi archived C++ deluge_app closure). Under
    // `--features host_app` we instead bindgen the host ABI and link the
    // host-built `deluge_app` object closure, letting the host binary reach
    // the C++ app boundary without a device build. Without that feature the
    // host path stays a no-op.
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("none") {
        if env::var("CARGO_FEATURE_HOST_APP").is_ok() {
            run_host_app(&repo_root, &manifest_dir, &out_dir);
        }
        return;
    }

    // ---------------------------------------------------------------------
    // bindgen: generate the libdeluge POD types from the canonical headers
    // (include/libdeluge/*.h). Types only — we DEFINE the service functions
    // ourselves in src/ffi.rs (#[no_mangle]); the C contract drives the types
    // so a layout/type change is a compile error.
    // ---------------------------------------------------------------------
    run_bindgen(&repo_root, &manifest_dir, &out_dir, "armv7a-none-eabihf");

    // ---------------------------------------------------------------------
    // Linker script + memory layout (rza1l-hal's build.rs puts rza1l.x on the
    // link search path; we supply the matching memory.x). Mirrors deluge-sdk's
    // firmwares/controller-firmware.
    // ---------------------------------------------------------------------
    let rtt = env::var("CARGO_FEATURE_RTT").is_ok();
    let (memory_src, linker_script) = if rtt {
        ("memory_rtt.x", "rza1l_rtt.x")
    } else {
        ("memory.x", "rza1l.x")
    };
    // Linker sources live under linker/ (NOT the crate root): the cross linker
    // resolves `INCLUDE memory.x` from its CWD (the crate root) before the -L
    // search path, so a memory.x in the root would shadow this generated one.
    let linker_src = manifest_dir.join("linker");
    fs::copy(linker_src.join(memory_src), out_dir.join("memory.x")).unwrap();
    // Supplementary fragment that places the app's SDRAM sections + symbols.
    fs::copy(
        linker_src.join("sdram_sections.x"),
        out_dir.join("sdram_sections.x"),
    )
    .unwrap();
    println!("cargo:rustc-link-search={}", out_dir.display());
    println!("cargo:rustc-link-arg=-Wl,-T,{linker_script}");
    println!("cargo:rustc-link-arg=-Wl,-T,sdram_sections.x");
    println!("cargo:rerun-if-changed=linker/memory.x");
    println!("cargo:rerun-if-changed=linker/memory_rtt.x");
    println!("cargo:rerun-if-changed=linker/sdram_sections.x");

    // No C++ caller of `deluge_sample_stream_*` exists yet — unlike `deluge_app_init` below,
    // nothing in this crate's own Rust code or the archived C++ closure has an unresolved
    // reference into `deluge_sample_stream`'s rlib, so ordinary lazy `.a` extraction would never
    // pull its object in at all and its `#[no_mangle]` symbols would be absent from the final ELF
    // even though the crate compiled clean. Force EACH of the six ABI entry points as a link root
    // (rustc passes `--gc-sections` by default, which prunes unreached function sections one at a
    // time even within an already-extracted object — a single `-u` root only keeps the one
    // function its own call graph reaches, so each symbol needs its own root here). Mirrors
    // `deluge_app_init`'s own `-u` just below, for the analogous reason on the C++ side.
    for sym in [
        "deluge_sample_stream_open",
        "deluge_sample_stream_close",
        "deluge_sample_stream_set_geometry",
        "deluge_sample_stream_get_asset_id",
        "deluge_sample_stream_set_asset_id",
        "deluge_sample_stream_read_at",
    ] {
        println!("cargo:rustc-link-arg=-Wl,-u,{sym}");
    }

    // Same reasoning again: `deluge_efatfs_remount` (efatfs_fs.rs) still has no C++ caller (only
    // Task 2's card-swap follow-up wires it in), so without this root `--gc-sections` would prune it
    // from the final link. `_mount`/`_cluster_size`/`_is_mounted` now have real callers via
    // storage_manager.cpp/audio_file_manager.cpp, but keeping them rooted here too is harmless.
    for sym in [
        "deluge_efatfs_mount",
        "deluge_efatfs_remount",
        "deluge_efatfs_cluster_size",
        "deluge_efatfs_is_mounted",
    ] {
        println!("cargo:rustc-link-arg=-Wl,-u,{sym}");
    }

    // ---------------------------------------------------------------------
    // Link the portable C++ application (built by CMake into the `build/` dir).
    // deluge_app is an OBJECT lib (no .a), so archive its objects here, then
    // link that + the static-lib closure as one --start-group (mutual refs:
    // C++ calls our deluge_* services, we call its deluge_main()).
    //
    // BRING-UP: assumes the Release config is already built in <root>/build
    // (`cmake --build build --target deluge_app fatfs NE10 …`). Parametrize the
    // build dir / config later; this is the two-step flow from the plan.
    // ---------------------------------------------------------------------
    let build_dir = env::var("DELUGE_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root.join("build"));
    // Bring-up uses Debug: Release compiles the app with -flto=auto (GCC slim-LTO
    // objects whose symbols rust's lld can't read). Debug objects are plain ELF
    // (and carry debug_info). Switch to Release later via bfd ld if LTO is wanted.
    let cfg = env::var("DELUGE_BUILD_CONFIG").unwrap_or_else(|_| "Debug".into());
    // `toolchain/current` symlinks to the active toolchain version's host dir,
    // so this survives version bumps (was a hardcoded, now-stale toolchain/v22).
    let ar = repo_root.join("toolchain/current/arm-none-eabi-gcc/bin/arm-none-eabi-ar");

    let app_objs_dir = build_dir.join(format!("src/deluge/CMakeFiles/deluge_app.dir/{cfg}"));
    if !app_objs_dir.is_dir() {
        panic!(
            "C++ app objects not found at {}. Build them first:\n  \
             cmake --build {} --target deluge_app fatfs NE10 eyalroz_printf \
             deluge_dsp deluge_scheduler deluge_foundation deluge_midi",
            app_objs_dir.display(),
            build_dir.display()
        );
    }

    // Collect all deluge_app .obj files and archive them into libdeluge_app_objs.a.
    let mut objs = Vec::new();
    collect_objs(&app_objs_dir, &mut objs);
    objs.sort();
    // Re-run (re-archive) when any object's CONTENT changes — a dir rerun-if-changed
    // only fires on add/remove, so editing a .cpp + rebuilding deluge_app wouldn't
    // otherwise re-archive, leaving a stale link.
    for o in &objs {
        println!("cargo:rerun-if-changed={}", o.display());
    }
    let app_objs_archive = out_dir.join("libdeluge_app_objs.a");
    let _ = fs::remove_file(&app_objs_archive);
    let status = Command::new(&ar)
        .arg("crs")
        .arg(&app_objs_archive)
        .args(&objs)
        .status()
        .expect("run arm-none-eabi-ar");
    assert!(status.success(), "archiving deluge_app objects failed");

    // The portable static-lib closure (argon/etl are header-only).
    let deps: [(&str, &str); 7] = [
        ("src/fatfs", "libfatfs.a"),
        ("src/NE10", "libNE10.a"),
        ("src/lib", "libeyalroz_printf.a"),
        ("src/deluge/dsp", "libdeluge_dsp.a"),
        ("src/OSLikeStuff", "libdeluge_scheduler.a"),
        ("src/foundation", "libdeluge_foundation.a"),
        ("src/midi", "libdeluge_midi.a"),
    ];

    // One link group so the mutual C++/Rust refs resolve.
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    println!("cargo:rustc-link-arg={}", app_objs_archive.display());
    for (dir, lib) in deps {
        let p = build_dir.join(dir).join(&cfg).join(lib);
        assert!(p.is_file(), "missing dep archive {}", p.display());
        println!("cargo:rustc-link-arg={}", p.display());
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");

    // C++/C runtime: rustc passes -nodefaultlibs, so re-add the g++ runtime the
    // app needs (libstdc++/libsupc++ for std::, __cxa_*, vtables; libgcc for
    // helpers like __popcountsi2; newlib libc/libm; libnosys for unhosted
    // syscall stubs). g++'s own search paths resolve these. Grouped for the
    // libstdc++<->libc<->libgcc circular refs.
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    for l in ["-lstdc++", "-lsupc++", "-lc", "-lm", "-lgcc", "-lnosys"] {
        println!("cargo:rustc-link-arg={l}");
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");

    // __exidx_start/__exidx_end are now defined by rza1l.x's .ARM.exidx section
    // (kept, not discarded) so C++ exception unwinding works.

    println!("cargo:rerun-if-changed={}", app_objs_dir.display());

    // Self-clearing nag for the temporary rza1l.x stack/heap-overlap workaround
    // (program_stack_start retargeted to __sram_heap_end so the C++ app's
    // GeneralMemoryAllocator can't overrun the mode stacks). Warns on every build
    // while the HACK tag is present in the sibling deluge-sdk linker scripts;
    // goes silent automatically once the proper fix (app sources heap bounds via
    // libdeluge/memory.h) removes it.
    let sdk = manifest_dir.join("../../../../deluge-sdk/crates/rza1l-hal");
    for s in ["rza1l.x", "rza1l_rtt.x"] {
        let p = sdk.join(s);
        println!("cargo:rerun-if-changed={}", p.display());
        if fs::read_to_string(&p).is_ok_and(|c| c.contains("DELUGE_APP_HEAP_HACK")) {
            println!(
                "cargo:warning=TEMP HACK active in deluge-sdk {s}: program_stack_start \
                 retargeted to __sram_heap_end for the C++ app heap. Remove once the app \
                 sources heap bounds via libdeluge/memory.h (deluge_memory_*)."
            );
        }
    }
}

/// Run bindgen over the canonical libdeluge headers (`include/libdeluge/*.h`)
/// for `clang_target`, writing `libdeluge_sys.rs` into `out_dir`. Shared by the
/// device path (`--target=armv7a-none-eabihf`) and the `host_app` path
/// (`--target=x86_64-unknown-linux-gnu`) — same allowlist/flags otherwise, so
/// the two `mod sys`es stay structurally identical modulo target.
fn run_bindgen(
    repo_root: &std::path::Path,
    manifest_dir: &std::path::Path,
    out_dir: &std::path::Path,
    clang_target: &str,
) {
    let include_dir = repo_root.join("include");
    let wrapper = manifest_dir.join("wrapper.h");
    let bindings = bindgen::Builder::default()
        .header(wrapper.to_str().unwrap())
        .clang_arg(format!("-I{}", include_dir.display()))
        // Types only (incl. the fn-pointer aliases). The service functions are
        // DEFINED in src/ffi.rs; emitting bindgen's `extern "C"` decls too would
        // trip edition-2024's "extern blocks must be unsafe" (bindgen 0.70).
        .allowlist_type("Deluge.*")
        .allowlist_type("RunCondition")
        .use_core()
        // NO `-fshort-enums`: it stays out of both bindgen paths (device and host_app)
        // deliberately, and is IRRELEVANT to enum sizing. Every one of the 11 libdeluge FFI enums
        // (DelugeInputEventKind, DelugeCardEvent, DelugeStatus, DelugeRegionState, …) pins its
        // underlying type explicitly in its header (e.g. `enum DelugeInputEventKind : uint8_t`,
        // `enum DelugeStatus : int8_t`) at its arm-none-eabi-gcc `-fshort-enums` width (1 byte,
        // all 11). An explicit underlying type is authoritative in both C and C++ — no compiler
        // flag or ABI default can override it — so bindgen sizes every one of these enums
        // identically on every target (arm device, x86_64 host_app, host stand-ins) regardless of
        // `-fshort-enums`.
        //
        // Warning: without an explicit underlying type, the arm device (which defaults to short
        // enums) and an un-flagged bindgen target would silently disagree on enum width,
        // mislaying out every enum-bearing POD (DelugeInputEvent, DelugeBoard, MIDI/card
        // events, …) across the FFI boundary. Point libclang at the actual target purely for
        // pointer width / alignment / calling convention.
        .clang_arg(format!("--target={clang_target}"))
        // Layouts now match the app being linked on every target (explicit
        // fixed-width enums everywhere); the asserts would run host-side
        // anyway.
        .layout_tests(false)
        .generate()
        .expect("bindgen failed on libdeluge headers");
    bindings
        .write_to_file(out_dir.join("libdeluge_sys.rs"))
        .expect("write libdeluge_sys.rs");
    println!("cargo:rerun-if-changed={}", wrapper.display());
    println!("cargo:rerun-if-changed={}", include_dir.display());
}

/// `host_app` feature: bindgen the host ABI (x86-64; every libdeluge enum is
/// pinned to an explicit fixed-width underlying type in its header, so this
/// matches build-embassy-hostapp's CMake config byte-for-byte regardless of
/// `-fshort-enums`, which neither build passes) into the real `mod sys`, then
/// archive the host-built C++ `deluge_app` object closure and emit link
/// directives so the crate reaches the linker against real provider-symbol
/// references.
fn run_host_app(
    repo_root: &std::path::Path,
    manifest_dir: &std::path::Path,
    out_dir: &std::path::Path,
) {
    // Switching which CMake tree we archive from (e.g. the plain
    // build-embassy-hostapp vs. a clang+TSan build-embassy-hostapp-tsan, see
    // HOST_HARNESS.md) must itself trigger a rerun: without this, Cargo has no
    // rerun-if-changed/rerun-if-env-changed directive from a PRIOR run that
    // mentions the new dir at all, so pointing DELUGE_HOSTAPP_BUILD_DIR
    // somewhere new can silently keep linking whatever was last archived.
    println!("cargo:rerun-if-env-changed=DELUGE_HOSTAPP_BUILD_DIR");

    run_bindgen(repo_root, manifest_dir, out_dir, "x86_64-unknown-linux-gnu");

    // This link is expected to fail on undefined provider symbols while the
    // host-side callers are still being wired up — lift lld's default error
    // cap so a single `cargo build` run surfaces the complete set instead of
    // truncating after the first batch.
    println!("cargo:rustc-link-arg=-Wl,--error-limit=0");
    // No host Rust code calls `deluge_app_init` yet. Force it as a link root
    // (`-u`) so lld extracts deluge.cpp.o from the archive and keeps its whole
    // transitively-reachable graph under the default --gc-sections — without
    // that, gc-sections would strip everything down to just the C++
    // global-constructor subset.
    println!("cargo:rustc-link-arg=-Wl,-u,deluge_app_init");
    // Same reasoning as the device path's identical block above — no C++ caller of
    // `deluge_sample_stream_*` exists yet, so without these roots `--gc-sections` (rustc's default)
    // would prune every one of its `#[no_mangle]` functions from the final link.
    for sym in [
        "deluge_sample_stream_open",
        "deluge_sample_stream_close",
        "deluge_sample_stream_set_geometry",
        "deluge_sample_stream_get_asset_id",
        "deluge_sample_stream_set_asset_id",
        "deluge_sample_stream_read_at",
    ] {
        println!("cargo:rustc-link-arg=-Wl,-u,{sym}");
    }
    // Same reasoning again: `deluge_efatfs_remount` (efatfs_host_shim.rs) still has no C++ caller —
    // see the device path's identical block above.
    for sym in [
        "deluge_efatfs_mount",
        "deluge_efatfs_remount",
        "deluge_efatfs_cluster_size",
        "deluge_efatfs_is_mounted",
    ] {
        println!("cargo:rustc-link-arg=-Wl,-u,{sym}");
    }

    // CMake-built host tree (`cmake -S sim -B build-embassy-hostapp
    // -DDELUGE_HOST_EMBASSY=... ; ninja -C build-embassy-hostapp deluge_app`).
    // Overridable so CI/devs can point at a differently-named build dir.
    let build_dir = env::var("DELUGE_HOSTAPP_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root.join("build-embassy-hostapp"));
    // Single-config Ninja generator (unlike the device path's multi-config
    // `.../{cfg}` layout) — objects land directly under deluge_app.dir, no
    // Debug/Release subdir.
    let app_objs_dir = build_dir.join("app/CMakeFiles/deluge_app.dir");
    if !app_objs_dir.is_dir() {
        panic!(
            "host C++ app objects not found at {}. Build them first:\n  \
             ninja -C {} deluge_app fatfs NE10 eyalroz_printf deluge_dsp \
             deluge_scheduler deluge_foundation deluge_midi",
            app_objs_dir.display(),
            build_dir.display()
        );
    }

    let mut objs = Vec::new();
    collect_objs(&app_objs_dir, &mut objs);
    objs.sort();
    // Re-archive on object content change, not just add/remove (see the device
    // path's identical rationale above).
    for o in &objs {
        println!("cargo:rerun-if-changed={}", o.display());
    }
    let app_objs_archive = out_dir.join("libdeluge_app_objs.a");
    let _ = fs::remove_file(&app_objs_archive);
    // Host `ar` (not arm-none-eabi-ar): these are x86-64 ELF objects.
    let status = Command::new("ar")
        .arg("crs")
        .arg(&app_objs_archive)
        .args(&objs)
        .status()
        .expect("run host ar");
    assert!(status.success(), "archiving host deluge_app objects failed");

    // This hash-based staleness check is load-bearing, not a defensive extra:
    // a reconfigured/rebuilt CMake tree (e.g. flipping on -fsanitize=thread)
    // whose objects Cargo's mtime-based `rerun-if-changed` failed to notice
    // gets the OUT_DIR archive above rebuilt this run from fresh objects, but
    // downstream the rustc-link-arg lines below are byte-identical to the
    // previous run (same archive path) — from Cargo's fingerprint's point of
    // view, "nothing about this build script's output changed", so it can
    // decide the final `deluge-rust` binary doesn't need relinking even
    // though `libdeluge_app_objs.a`'s CONTENT just changed underneath that
    // unchanged path. Hash the actual object closure and thread the hash
    // through `cargo:rustc-env`: Cargo diffs a build script's full emitted
    // metadata (rustc-env/rustc-cfg/rustc-link-*) run over run, so a changed
    // hash value forces this crate — and therefore the final link — to be
    // considered stale and rebuilt, independent of whether any individual
    // `.o`'s mtime was itself trusted. This directly targets the failure mode
    // where an instrumented `.o` sits on disk but an uninstrumented archive is
    // still linked in, without requiring a `target/` wipe.
    let content_hash = hash_objs_content(&objs);
    let hash_str = format!("{content_hash:016x}");
    let hash_stamp = out_dir.join("host_app_objs_hash.txt");
    let prev_hash = fs::read_to_string(&hash_stamp).ok();
    if prev_hash.as_deref() != Some(hash_str.as_str()) {
        println!(
            "cargo:warning=host_app: deluge_app object closure at {} changed ({} -> {}); forcing a relink",
            app_objs_dir.display(),
            prev_hash.as_deref().unwrap_or("<none>"),
            hash_str
        );
    }
    fs::write(&hash_stamp, &hash_str).expect("write host_app_objs_hash.txt");
    println!("cargo:rustc-env=DELUGE_APP_OBJS_HASH={hash_str}");

    // The portable static-lib closure, built alongside deluge_app in the same
    // host tree (see the panic message above). Paths mirror build-embassy-hostapp's
    // actual layout (NE10 at the build root, dsp under app/, not src/deluge/ —
    // both differ from the device tree's layout; see collect step above).
    let deps: [(&str, &str); 7] = [
        ("fatfs", "libfatfs.a"),
        (".", "libNE10.a"),
        ("printf", "libeyalroz_printf.a"),
        ("app/dsp", "libdeluge_dsp.a"),
        ("scheduler", "libdeluge_scheduler.a"),
        ("foundation", "libdeluge_foundation.a"),
        ("midi", "libdeluge_midi.a"),
    ];

    // One link group so the mutual C++/Rust refs resolve (the `-u` above is
    // what actually pulls deluge_app_init — and everything it transitively
    // reaches — out of this archive; see the comment there).
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    println!("cargo:rustc-link-arg={}", app_objs_archive.display());
    for (dir, lib) in deps {
        let p = build_dir.join(dir).join(lib);
        assert!(p.is_file(), "missing host dep archive {}", p.display());
        println!("cargo:rustc-link-arg={}", p.display());
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");

    // Host runtime: rustc passes -nodefaultlibs, so re-add what the app needs
    // (libstdc++ for std::/vtables, libgcc for compiler helpers, libc/libm).
    // Unlike the device (arm-eabi/newlib) group, host glibc pulls libsupc++ in
    // via libstdc++ and needs no unhosted syscall stubs, so NO -lsupc++/-lnosys
    // here. Grouped for the libstdc++<->libc<->libgcc circular refs.
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    for l in ["-lstdc++", "-lm", "-lc", "-lgcc"] {
        println!("cargo:rustc-link-arg={l}");
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");

    println!("cargo:rerun-if-changed={}", app_objs_dir.display());
}

/// Content hash over a sorted object closure (path + bytes of each `.o`), used
/// by the `host_app` path to force a relink whenever the archived objects'
/// CONTENT changes even if Cargo's own `rerun-if-changed` mtime tracking of
/// the individual files does not (see the staleness comment at the call
/// site). Not cryptographic — `DefaultHasher` (SipHash) is fine for a
/// same-machine, same-run change/no-change signal; the whole closure is
/// tens of MB and hashes in well under a second.
fn hash_objs_content(objs: &[PathBuf]) -> u64 {
    use std::hash::Hasher;
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    for o in objs {
        hasher.write(o.to_string_lossy().as_bytes());
        let bytes = fs::read(o)
            .unwrap_or_else(|e| panic!("failed to read {} for staleness hash: {e}", o.display()));
        hasher.write(&bytes);
    }
    hasher.finish()
}

fn collect_objs(dir: &std::path::Path, out: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(dir).unwrap() {
        let p = entry.unwrap().path();
        if p.is_dir() {
            collect_objs(&p, out);
        } else if p.extension().is_some_and(|e| e == "obj" || e == "o") {
            out.push(p);
        }
    }
}
