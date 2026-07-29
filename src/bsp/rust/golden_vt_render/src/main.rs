//! `golden_vt_render`: an Embassy-host-BSP golden renderer. Boots the real C++
//! `deluge_app` on this repo's Rust/Embassy host substrate (the same
//! `raw::Executor` + `PeekableMockDriver` + no-thread pender shape
//! `../lens1_vt_sim/` pioneered for the streaming-underrun harness), mounts a
//! fixture SD-card image, and — **this rung's scope only** — exits cleanly.
//! Later rungs replace [`run_stem_export_scenario`]'s stub body with a real
//! `StemExport` drive and wire up deterministic stem-audio capture; this file
//! only takes the harness as far as boot + mount + a clean `exit(0)`.
//!
//! # Why a sibling package, not a `lens1_vt_sim` `[[bin]]`
//!
//! `embassy-time`'s `std` feature (`deluge-bsp-rust`'s own host target) and a
//! custom/`mock-driver`-shaped time driver both define `#[no_mangle]
//! _embassy_time_now`/`_embassy_time_schedule_wake` — a hard duplicate-symbol
//! compile error if both land in one package's dependency graph (Cargo unifies
//! a dependency's features across every target in one Cargo.toml; there is no
//! per-`[[bin]]` override). See `Cargo.toml`'s header comment for the full
//! rationale — identical to `../lens1_vt_sim/Cargo.toml`'s.
//!
//! # Executor + clock
//!
//! `embassy_executor::raw::Executor` + a custom `AtomicBool` `__pender` + a
//! `PeekableMockDriver` ([`clock`] — the same discrete-event shape
//! `../lens1_vt_sim/src/clock.rs`/`../spike_mock_clock/` use). The driver loop:
//! poll the executor to quiescence, peek the next due deadline, jump the
//! virtual clock to exactly that deadline, repeat. No `platform-std`/
//! `executor-thread` embassy-executor feature is enabled (see `Cargo.toml`),
//! so there is exactly one `__pender` in this binary and it is ours.
//!
//! # The boot livelock (see `sd.rs`'s `sim_latency::off_fiber_instant` doc
//! comment for the full mechanism, and `../lens1_vt_sim/src/main.rs`'s
//! identical module-doc section for the original write-up)
//!
//! `deluge_app_init` calls the boot-time FatFS mount SYNCHRONOUSLY, off the
//! worker fiber, via `embassy_futures::block_on` — a tight busy-spin poll loop
//! that never returns control to anything else on this one OS thread. Under
//! `sim_latency`, that polled future's completion depends on a separately
//! spawned `pump()` task's `Timer::after(latency).await` being POLLED by the
//! executor — but the executor's own `poll()` call is already on the stack, so
//! nothing else can be polled until that whole call stack unwinds.
//! `sd::sim_latency::set_off_fiber_instant` (called once in `main`, before
//! anything spawns) sidesteps this: an off-fiber modeled transfer skips the
//! modeled delay entirely and goes straight to the real (synchronous) read, so
//! there is nothing for the busy-spin to wait on.
//!
//! # SD-image packing happens BEFORE any task is spawned
//!
//! `sd_image::pack_golden_fixture` + setting `DELUGE_SD_IMAGE` run
//! synchronously in `main`, before the executor exists or any task is
//! spawned — deliberately NOT inside [`run_stem_export_scenario`] itself,
//! even though that task is conceptually "the one that packs the fixture".
//! `deluge-bsp`'s host `sd.rs` resolves `DELUGE_SD_IMAGE` lazily, exactly
//! once, behind a `OnceLock` (`disk()`) the first time anything touches the
//! backing file — and [`scenario::run`]'s own doc comment already documents
//! the relevant hazard: "embassy doesn't guarantee poll order across tasks
//! spawned in the same batch". If packing+env-var-set instead lived inside
//! [`run_stem_export_scenario`]'s task body, a poll order that reaches
//! [`boot_task`]'s `sd::init()` first would lock the `OnceLock` onto the
//! WRONG (default) backing path before the real fixture image is ready —
//! silently, not a crash. Doing it in `main` (single-threaded, before the
//! executor exists at all) has no such race, matching `../lens1_vt_sim/src/main.rs`'s
//! own proven-safe placement of the identical call.
//!
//! # Reused shared substrate
//!
//! `#[path]`-includes the SAME source files `deluge-bsp-rust`'s own `host_app`
//! build (and `../lens1_vt_sim/`) compiles (`fiber.rs`, `scheduler.rs`, `sd.rs`,
//! `board.rs`, `control.rs`, `display.rs`, `ffi.rs`, `ffi_extra.rs`, `flash.rs`,
//! `services.rs`, `audio_host.rs`, `sd_image.rs`, `streaming_loader.rs`,
//! `efatfs_core.rs`, `efatfs_host_shim.rs`) — a second, independently
//! configured COMPILATION of the same source (this package's own `mod
//! sys`/Cargo features), not a fork: a change to any of these files is picked
//! up by every package that path-includes it. Deliberately does NOT
//! `#[path]`-include `scenario.rs` (unlike `../lens1_vt_sim/`): nothing this
//! rung boots reaches into it, and this package's own future scenario
//! ([`run_stem_export_scenario`]) is a `StemExport` drive, not the
//! streaming-underrun harness's play/record scenario that file wraps.
#![feature(impl_trait_in_assoc_type)]

use core::sync::atomic::{AtomicBool, Ordering};

use embassy_executor::raw;
use embassy_time::{Duration, Instant, Timer};
use embassy_time_driver::Driver as _;

mod clock;

// Link-only: the host-built C++ `deluge_app` object closure (build.rs) calls the
// deluge_resource_* residency C ABI and the deluge_{alloc,slab_*,heap_*}
// allocator C ABI. `extern crate` forces the rlib onto the link line so those
// `#[no_mangle]` symbols resolve; nothing here references them from Rust
// (mirrors `../lens1_vt_sim/src/main.rs`'s identical `host_app`-gated
// declaration).
extern crate deluge_resource;
// Link-only, same reasoning as `deluge_resource` above: `sample_source.cpp`/
// `sample_reader_bridge.cpp` in the archived `deluge_app` object closure call
// straight into these crates' `deluge_sample_reserve_*`/`deluge_sample_peek`/
// `deluge_sample_read`/`deluge_sample_invalidate`/`deluge_sample_reader_*`
// `#[no_mangle]` symbols; nothing here references them from Rust, so without
// this `extern crate` rustc/lld would drop the whole rlib and the link would
// fail with undefined symbols (mirrors `../src/main.rs`'s identical
// `host_app`-gated declarations for both crates).
extern crate deluge_sample_reader;
extern crate deluge_sample_source;

/// Real bindgen'd libdeluge C-ABI types (see `build.rs`).
#[allow(
    non_upper_case_globals,
    non_camel_case_types,
    non_snake_case,
    dead_code
)]
mod sys {
    include!(concat!(env!("OUT_DIR"), "/libdeluge_sys.rs"));
}

// Reuse deluge-bsp-rust's own source files verbatim via `#[path]` — the same
// pattern `../lens1_vt_sim/`/`tests/*.rs` already use (see `HOST_HARNESS.md`).
#[path = "../../src/audio_host.rs"]
mod audio_host;
#[path = "../../src/board.rs"]
mod board;
#[path = "../../src/control.rs"]
mod control;
#[path = "../../src/display.rs"]
mod display;
/// Storage-generic core of the efatfs read path — see `../../src/main.rs`'s
/// `mod efatfs_core` doc. Gated on `host_app` (always on for this package) to
/// mirror that file's cfg exactly, even though this package's own
/// `#[cfg(feature = "efatfs_streaming")]` is what actually drives `mount()`
/// (below, in `boot_task`).
#[cfg(feature = "host_app")]
#[path = "../../src/efatfs_core.rs"]
mod efatfs_core;
/// Host counterpart of the device `efatfs_fs.rs` — see `../../src/main.rs`'s
/// `mod efatfs_host_shim` doc.
#[cfg(feature = "host_app")]
#[path = "../../src/efatfs_host_shim.rs"]
mod efatfs_host_shim;
#[path = "../../src/ffi.rs"]
mod ffi;
#[path = "../../src/ffi_extra.rs"]
mod ffi_extra;
#[path = "../../src/fiber.rs"]
mod fiber;
#[path = "../../src/flash.rs"]
mod flash;
#[path = "../../src/host_link_stubs.rs"]
mod host_link_stubs;
#[path = "../../src/scheduler.rs"]
mod scheduler;
#[path = "../../src/sd.rs"]
mod sd;
#[path = "../../src/sd_image.rs"]
mod sd_image;
#[path = "../../src/services.rs"]
mod services;
/// The async cluster-fill task + its selector/wakeup C ABI. The
/// selector/wakeup symbols (`deluge_streaming_async_active`/
/// `deluge_streaming_signal_fill`) are always compiled so `loader.cpp`'s call
/// sites link regardless of the `async_streaming_loader` feature (see that
/// file's module doc); the actual drain machinery +
/// [`streaming_loader::streaming_fill_task`] stay gated on the feature and are
/// only spawned (below, in `main`) when it's enabled — on by default for this
/// package (see `Cargo.toml`).
#[path = "../../src/streaming_loader.rs"]
mod streaming_loader;

unsafe extern "C" {
    fn deluge_app_init(board: *const sys::DelugeBoard);
}

/// Set once [`boot_task`] has run the efatfs mount, `deluge_app_init` (which
/// itself runs the C FatFS mount synchronously), and is about to enter its
/// worker-fiber pump loop. This rung's [`run_stem_export_scenario`] waits on
/// this flag and then declares the run done — the entirety of Task 1's scope
/// (boot + mount + clean exit, no scenario logic yet).
static BOOT_MOUNTED: AtomicBool = AtomicBool::new(false);

/// Boot task: mirrors `deluge-bsp-rust`'s `host_app_task` exactly (PIC-ready
/// wait, SD bring-up, efatfs mount, `deluge_app_init`, worker-fiber pump loop)
/// — see `../lens1_vt_sim/src/main.rs`'s `boot_task` doc comment for the full
/// rationale behind each step; this is the same ~15-line reimplementation (not
/// `#[path]`-included for the same reason: `host_app_task` lives inside
/// `../src/main.rs` itself, which carries a conflicting `#![no_std]`/device
/// `fn main`/`EXECUTOR` static).
#[embassy_executor::task]
async fn boot_task() {
    deluge_bsp::pic::wait_ready().await;
    crate::sd::boot_init().await;

    // Mirrors `deluge-bsp-rust`'s `host_app_task` efatfs mount (see its
    // comment). `main`'s `sd::sim_latency::set_off_fiber_instant(true)`
    // (already set once before anything is spawned) covers this mount exactly
    // like it covers the C-FatFS mount inside `deluge_app_init` below: this
    // mount's block device goes through the same `deluge_block_read` off-fiber
    // dispatch (see `efatfs_host_shim.rs`'s module doc), so it inherits the
    // flag with no extra wrap/restore needed here. A failed mount must NOT
    // abort boot — see the device/host_app_task comments this mirrors.
    #[cfg(feature = "efatfs_streaming")]
    match crate::efatfs_host_shim::mount().await {
        Ok(()) => log::info!("golden_vt_render: efatfs mounted"),
        Err(()) => {
            log::warn!("golden_vt_render: efatfs mount failed — streaming falls back to C FatFS");
        }
    }

    log::info!("golden_vt_render: deluge_app_init() (registers + spawns task runners)");
    // SAFETY: called once, after `scheduler::set_spawner` (main, below) and
    // before anything else touches `currentSong`/scheduler state.
    unsafe { deluge_app_init(board::deluge_board()) };
    BOOT_MOUNTED.store(true, Ordering::Release);
    log::info!("golden_vt_render: boot+mount complete; scheduler running; pumping async worker");

    use embassy_futures::select::select;
    loop {
        let busy = fiber::worker_poll();
        if busy {
            let _ = select(
                fiber::WORKER_WAKE.wait(),
                embassy_time::Timer::after_millis(8),
            )
            .await;
        } else {
            fiber::WORKER_WAKE.wait().await;
        }
    }
}

/// This rung's scenario: waits for [`boot_task`] to finish booting + mounting,
/// then declares the run done. `fixture` is already packed and pointed at by
/// `DELUGE_SD_IMAGE` before this task is even spawned (see the module doc's
/// "SD-image packing" section) — it's threaded through here only so the log
/// line identifies which fixture this run booted against, and so later rungs
/// (which replace this body with a real `StemExport` drive) have it in scope
/// without a signature change.
///
/// Deliberately does NOT call any `StemExport` C-ABI yet — that's a later
/// rung's job (see the module doc).
#[embassy_executor::task]
async fn run_stem_export_scenario(fixture: &'static str, done: &'static AtomicBool) {
    log::info!("golden_vt_render: run_stem_export_scenario: fixture={fixture}, awaiting boot+mount");
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        if BOOT_MOUNTED.load(Ordering::Acquire) {
            break;
        }
        if Instant::now() >= deadline {
            log::error!(
                "golden_vt_render: boot+mount did not complete within the 30s wait budget — wedged"
            );
            hard_exit(2);
        }
        Timer::after_millis(5).await;
    }
    log::info!("golden_vt_render: boot+mount confirmed for fixture={fixture}; exiting cleanly");
    done.store(true, Ordering::Release);
}

/// Our own pender: no thread, no parking — just a flag the driver loop polls
/// itself between `raw::Executor::poll()` calls.
static PENDED: AtomicBool = AtomicBool::new(false);

#[unsafe(export_name = "__pender")]
fn pender(_context: *mut ()) {
    PENDED.store(true, Ordering::SeqCst);
}

fn main() {
    // See `../lens1_vt_sim/src/main.rs`'s identical NOTE: a `--release` build
    // of this package also compiles every `log::*!` call to a no-op
    // (`deluge-bsp`/`rza1l-hal` request `log`'s `release_max_level_off`
    // feature for the DEVICE build's benefit, and Cargo unifies that feature
    // across this whole binary's dependency graph). Use a debug build (`cargo
    // build`, no `--release`) when you need the log output.
    env_logger::init();
    log::info!("golden_vt_render: Embassy-host-BSP golden renderer (boot + mount rung)");

    let fixture = std::env::var("GOLDEN_FIXTURE").unwrap_or_else(|_| "cordae".to_string());
    // Bound on VIRTUAL time simulated before giving up (NOT wall time — see
    // `drive`'s doc comment below): generous, since a wedged boot should fail
    // loudly rather than "simulate" forever.
    let budget_ms: u64 = std::env::var("GOLDEN_VIRTUAL_BUDGET_MS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(60_000);

    log::info!("golden_vt_render: fixture={fixture} budget_ms={budget_ms}");

    // Pack (or reuse) a real FAT SD image from the golden corpus — same
    // tooling `../lens1_vt_sim/`'s scenario driver uses. Must run before
    // anything below touches SD, and MUST run here (single-threaded, before
    // any task is spawned) rather than inside a task — see the module doc's
    // "SD-image packing happens BEFORE any task is spawned" section.
    let repo_root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(4)
        .expect("CARGO_MANIFEST_DIR (src/bsp/rust/golden_vt_render) has a repo root 4 levels up")
        .to_path_buf();
    if std::env::var_os("DELUGE_SD_IMAGE").is_none() {
        let img = sd_image::pack_golden_fixture(&repo_root, &fixture);
        // SAFETY: single-threaded at this point (before any task/executor exists).
        unsafe { std::env::set_var("DELUGE_SD_IMAGE", &img) };
    }

    // See this file's module doc: sidesteps the boot-time off-fiber block_on
    // livelock. Set once, before anything spawns.
    sd::sim_latency::set_off_fiber_instant(true);
    // Zero-jitter starvation guard (see `fiber.rs`'s `HIGH_PRIORITY_FAIRNESS_BOUND`
    // doc comment): without this, `loader::request_pump`'s HIGH-priority
    // re-enqueue could starve NORMAL-priority dispatch forever on this virtual
    // clock. Same value `../lens1_vt_sim/` uses.
    fiber::set_high_priority_fairness_bound(8);

    // SAFETY: `raw::Executor` needs `&'static`; leaking a Box is the standard
    // pattern for a `fn main` that doesn't itself run forever
    // (`embassy_executor::Executor::run`'s own doc uses the same pattern).
    let executor: &'static raw::Executor =
        Box::leak(Box::new(raw::Executor::new(std::ptr::null_mut())));
    let spawner = executor.spawner();
    scheduler::set_spawner(spawner);

    // Same BSP task set `deluge-bsp-rust`'s host_app boot block /
    // `../lens1_vt_sim/` spawns (minus the audio-thread split — with
    // `scheduler::set_audio_spawner` never called, the priority-0 audio task
    // falls back to this single spawner automatically, the documented
    // fallback path in `scheduler.rs`'s `claim`).
    spawner.spawn(control::pic_pump().unwrap());
    spawner.spawn(control::pad_render().unwrap());
    spawner.spawn(control::encoder_wake_pump().unwrap());
    spawner.spawn(display::oled_render().unwrap());
    spawner.spawn(sd::sim_latency::pump().unwrap());
    spawner.spawn(boot_task().unwrap());
    // The async cluster-fill task, on the SAME executor `boot_task`'s
    // worker-fiber pump loop and the C++ enqueue path run on (this binary has
    // only the one executor — see the module doc's "Executor + clock"). Owns
    // the loader queue only once `deluge_streaming_async_active()` reports
    // true (i.e. only under this feature — on by default for this package);
    // inert otherwise.
    #[cfg(feature = "async_streaming_loader")]
    spawner.spawn(streaming_loader::streaming_fill_task().unwrap());

    let fixture_static: &'static str = Box::leak(fixture.into_boxed_str());
    static DONE: AtomicBool = AtomicBool::new(false);
    spawner.spawn(run_stem_export_scenario(fixture_static, &DONE).unwrap());

    // --- Discrete-event driver loop -----------------------------------------
    // Same shape as `../lens1_vt_sim/src/main.rs`'s driver loop: poll to
    // quiescence, then jump the virtual clock straight to the next due
    // deadline (rather than stepping tick-by-tick) until the scenario signals
    // done or the virtual-time budget/wedge detection trips.
    let driver = clock::PeekableMockDriver::get();
    let budget_ticks = Duration::from_millis(budget_ms).as_ticks();
    let wall_start = std::time::Instant::now();
    let mut quiescence_passes = 0u64;
    let mut advances = 0u64;
    loop {
        loop {
            PENDED.store(false, Ordering::SeqCst);
            // SAFETY: single-threaded, never called reentrantly.
            unsafe { executor.poll() };
            quiescence_passes += 1;
            if !PENDED.load(Ordering::SeqCst) {
                break;
            }
        }

        if DONE.load(Ordering::Acquire) {
            log::info!(
                "golden_vt_render: run_stem_export_scenario completed at virtual t={}us \
                 ({quiescence_passes} quiescence passes, {advances} clock advances)",
                driver.now(),
            );
            break;
        }

        let Some(next) = driver.peek_next_deadline() else {
            log::error!(
                "golden_vt_render: no task has an outstanding timer — boot/mount wedged (deadlock)"
            );
            hard_exit(2);
        };
        if next > budget_ticks {
            log::error!(
                "golden_vt_render: virtual-time budget ({budget_ms}ms) exhausted before boot/mount \
                 completed (next deadline at {next}us > budget {budget_ticks}us) — wedged"
            );
            hard_exit(2);
        }
        let now = driver.now();
        driver.advance_ticks(next - now);
        advances += 1;
    }
    let wall_elapsed = wall_start.elapsed();
    log::info!("golden_vt_render: wall-clock elapsed: {wall_elapsed:?}");
    log::info!("golden_vt_render: boot + mount succeeded; exiting cleanly");

    hard_exit(0);
}

/// Immediate, unconditional process termination — skips libc's atexit-run C++
/// static destructors. Same rationale (and same raw `exit_group` shape) as
/// `../lens1_vt_sim/src/main.rs`'s `hard_exit`: the real C++ app links objects
/// with static storage duration (`AudioEngine`, `playbackHandler`, …) that
/// register `__cxa_atexit` destructors at process start. A normal
/// `std::process::exit` still runs those destructors — observed elsewhere as a
/// SIGSEGV (exit code 139) AFTER the run had already succeeded, i.e.
/// destructor ordering against some other still-referenced global, not a bug
/// in the scenario itself. This process never needs an orderly C++ shutdown
/// (there is no real device shutdown path either), so skip it.
///
/// Flushes stdout/stderr first: `log`/`env_logger` (via its `anstream`
/// dependency) buffers writes rather than flushing per line, so without this,
/// a run that fails fast could exit with its diagnostic log output silently
/// discarded.
fn hard_exit(code: i32) -> ! {
    use std::io::Write as _;
    let _ = std::io::stdout().flush();
    let _ = std::io::stderr().flush();
    #[cfg(target_os = "linux")]
    unsafe {
        core::arch::asm!(
            "syscall",
            in("rax") 231usize, // exit_group
            in("rdi") code,
            options(noreturn, nostack)
        );
    }
    #[cfg(not(target_os = "linux"))]
    std::process::exit(code);
}
