//! Lens 1 (streaming-underrun harness, Task 7): a single-threaded, DETERMINISTIC
//! virtual-time discrete-event simulation of the real `host_app` (song load,
//! sustained playback, concurrent record), measuring `loaded`-miss underruns
//! (`deluge::harness::noteUnderrunWait`/`noteUnderrunUnassign`) as a function of
//! modeled SD latency / audio compute budget — reproducibly, independent of host
//! CPU speed.
//!
//! # Executor + clock (Spike A's shape, `.superpowers/sdd/task-1-report.md`)
//!
//! `embassy_executor::raw::Executor` + a custom `AtomicBool` `__pender` + a
//! `PeekableMockDriver` ([`clock`] — the exact shape Spike A proved in
//! `../spike_mock_clock/`). The driver loop: poll the executor to quiescence,
//! peek the next due deadline, jump the virtual clock to exactly that deadline,
//! repeat. No `platform-std`/`executor-thread` embassy-executor feature is
//! enabled (see `Cargo.toml`), so there is exactly one `__pender` in this binary
//! and it is ours.
//!
//! # The boot livelock (the hardest part — see `sd.rs`'s
//! `sim_latency::off_fiber_instant` doc comment for the full mechanism)
//!
//! `deluge_app_init` calls the boot-time FatFS mount SYNCHRONOUSLY, off the
//! worker fiber, via `embassy_futures::block_on` — a tight busy-spin poll loop
//! (confirmed by reading `embassy-futures` 0.1.2's actual source: `loop { if let
//! Ready(v) = fut.poll(cx) { return v } }`, no thread parking, no waker use at
//! all) that never returns control to anything else on this one OS thread. Under
//! `sim_latency`, that polled future is `sim_latency::modeled_read`, whose
//! completion depends on a separately spawned `pump()` task's
//! `Timer::after(latency).await` being POLLED by the executor — but the
//! executor's own `poll()` call is already on the stack (it's the one polling
//! the boot task, which is itself inside the synchronous `deluge_app_init` call,
//! which is inside `block_on`'s spin loop): nothing else can be polled until
//! that whole call stack unwinds. Lens 2 (`../src/main.rs`'s `host_app` block)
//! sidesteps this by running `pump` on a second real OS thread — not available
//! here (single-threaded, virtual clock; a real thread would reintroduce
//! wall-clock waiting and a second executor outside this loop's quiescence
//! check, defeating determinism — see the task brief).
//!
//! Fix: `sd.rs`'s `sim_latency::set_off_fiber_instant` (a small, additive,
//! off-by-default change to that SHARED file) makes an off-fiber modeled
//! transfer skip the modeled delay entirely and go straight to the real
//! (synchronous) read — no `Timer`/`pump` involved at all, so there is nothing
//! for the busy-spin to wait ON. Safe/faithful because the only off-fiber
//! transfers that can ever occur here are the boot-time FatFS mount plus
//! whatever essential-sample reads `deluge_app_init` issues synchronously
//! before the fiber exists — Task 6's report already established "load's own
//! essential-sample reads have no real-time deadline the underrun counters care
//! about"; this extends that reasoning one step earlier, to the mount itself.
//! Every transfer AFTER the fiber exists (song load's essential-sample fetches,
//! sustained streaming, recording) is genuinely ON-fiber (`block_on_fiber`, a
//! coroutine YIELD — not a busy spin: it suspends the fiber and returns control
//! to whoever called `fiber::worker_poll()`, which `.await`s normally
//! afterwards) — modeled latency applies there exactly as it does for Lens 2,
//! driven correctly by this binary's own single executor via the SAME
//! `sim_latency::pump` task, spawned once at boot.
//!
//! # Reused shared substrate
//!
//! `#[path]`-includes the SAME source files `deluge-bsp-rust`'s own `host_app`
//! build compiles (`fiber.rs`, `scheduler.rs`, `sd.rs`, `board.rs`, `control.rs`,
//! `display.rs`, `ffi.rs`, `ffi_extra.rs`, `flash.rs`, `services.rs`,
//! `audio_host.rs`, `scenario.rs`, `sd_image.rs`) — a second, independently
//! configured COMPILATION of the same source (this package's own `mod
//! sys`/Cargo features), not a fork: a change to any of these files is picked
//! up by both packages. The only shared-file BEHAVIOR changes are the two
//! small, additive, off-by-default hooks this file calls below
//! (`sd::sim_latency::set_off_fiber_instant`,
//! `scheduler::set_audio_period_override_us`) — both documented at their
//! definitions in `sd.rs`/`scheduler.rs`.
#![feature(impl_trait_in_assoc_type)]

use core::sync::atomic::{AtomicBool, Ordering};

use embassy_executor::raw;
use embassy_time::Duration;
use embassy_time_driver::Driver as _;

mod clock;

// Link-only: the host-built C++ `deluge_app` object closure (build.rs) calls the
// deluge_resource_* residency C ABI and the deluge_{alloc,slab_*,heap_*}
// allocator C ABI. `extern crate` forces the rlib onto the link line so those
// `#[no_mangle]` symbols resolve; nothing here references them from Rust
// (mirrors `../src/main.rs`'s identical `host_app`-gated declaration).
extern crate deluge_resource;

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
// pattern `tests/*.rs` already use for `fiber.rs`/`sd.rs` (see `HOST_HARNESS.md`).
#[path = "../../src/audio_host.rs"]
mod audio_host;
#[path = "../../src/board.rs"]
mod board;
#[path = "../../src/control.rs"]
mod control;
#[path = "../../src/display.rs"]
mod display;
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
#[path = "../../src/scenario.rs"]
mod scenario;
#[path = "../../src/scheduler.rs"]
mod scheduler;
#[path = "../../src/sd.rs"]
mod sd;
#[path = "../../src/sd_image.rs"]
mod sd_image;
#[path = "../../src/services.rs"]
mod services;

unsafe extern "C" {
    fn deluge_app_init(board: *const sys::DelugeBoard);
}

// Host-only harness underrun-counter readers (`harness/streaming_underrun.h`) —
// same manual-declaration pattern `scenario.rs` already uses (harness-only
// exports, not part of the bindgen'd libdeluge ABI).
unsafe extern "C" {
    fn deluge_sim_underrun_wait_count() -> u64;
    fn deluge_sim_underrun_unassign_count() -> u64;
}

/// `AudioEngine::routine_task`'s virtual per-block cadence: exactly one
/// 128-frame block's worth of real time, in microseconds — the brief's "128 /
/// 44100 s per block". This is the default `scheduler::set_audio_period_override_us`
/// value (the "compute-budget" knob); overridable via `LENS1_AUDIO_BLOCK_US` for
/// a later margin sweep (Task 8).
const DEFAULT_AUDIO_BLOCK_PERIOD_US: u64 = 128 * 1_000_000 / 44_100; // 2902 (floor)

/// Boot task: mirrors `deluge-bsp-rust`'s `host_app_task` exactly (PIC-ready
/// wait, SD bring-up, `deluge_app_init`, worker-fiber pump loop) — see
/// `../src/main.rs`'s `host_app_task` doc comment for the full rationale behind
/// each step. Not `#[path]`-included: `host_app_task` is defined INSIDE
/// `../src/main.rs` itself (not a separate module file), which also carries a
/// conflicting `#![no_std]`/device `fn main`/`EXECUTOR` static — there is no
/// clean way to pull just the one function in via `#[path]`. This is a direct,
/// small (~15-line) reimplementation of that sequence, not a duplicated
/// abstraction.
#[embassy_executor::task]
async fn boot_task() {
    deluge_bsp::pic::wait_ready().await;
    crate::sd::boot_init().await;

    log::info!("lens1-vt-sim: deluge_app_init() (registers + spawns task runners)");
    // SAFETY: called once, after `scheduler::set_spawner` (main, below) and
    // before anything else touches `currentSong`/scheduler state.
    unsafe { deluge_app_init(board::deluge_board()) };
    log::info!("lens1-vt-sim: scheduler running; pumping async worker");

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

/// Runs [`scenario::run`], stashes the result, and flags `done`. A thin task
/// wrapper — `run` itself is thread-agnostic (see `scenario.rs`'s module doc)
/// and makes no assumption about which executor drives it; this is just this
/// binary's way of getting its `Future` onto the one `raw::Executor` and
/// getting the result back out to `main`'s driver loop (which cannot `.await`).
#[embassy_executor::task]
async fn scenario_runner(cfg: scenario::ScenarioConfig, done: &'static AtomicBool) {
    log::info!("lens1-vt-sim: scenario_runner: starting scenario::run");
    let result = scenario::run(cfg).await;
    log::info!(
        "lens1-vt-sim: scenario result: boot_ready={} song_load_dispatched={} \
         listing_completed={} load_committed={} load_completed={} playback_started={} \
         playback_confirmed_active={} recording_started={} blocks_rendered={} \
         cluster_reads={} recorder_writes={} underrun_wait={} underrun_unassign={}",
        result.boot_ready,
        result.song_load_dispatched,
        result.listing_completed,
        result.load_committed,
        result.load_completed,
        result.playback_started,
        result.playback_confirmed_active,
        result.recording_started,
        result.blocks_rendered,
        result.cluster_reads,
        result.recorder_writes,
        result.underrun_wait,
        result.underrun_unassign,
    );
    *RESULT.lock().unwrap() = Some(result);
    done.store(true, Ordering::Release);
}

static RESULT: std::sync::Mutex<Option<scenario::ScenarioResult>> = std::sync::Mutex::new(None);

/// Our own pender: no thread, no parking — just a flag the driver loop polls
/// itself between `raw::Executor::poll()` calls (Spike A's shape).
static PENDED: AtomicBool = AtomicBool::new(false);

#[unsafe(export_name = "__pender")]
fn pender(_context: *mut ()) {
    PENDED.store(true, Ordering::SeqCst);
}

fn main() {
    // NOTE for anyone chasing "why did RUST_LOG=trace print nothing": `deluge-bsp`/`rza1l-hal`
    // (pulled in for their `sd`/`pic` host surface) both request `log`'s `release_max_level_off`
    // feature for the DEVICE build's benefit — Cargo unifies that feature across this whole
    // binary's dependency graph, so a `--release` build of THIS package also compiles every
    // `log::*!` call to a no-op (`log::STATIC_MAX_LEVEL` is `Off`), regardless of `RUST_LOG`.
    // `println!`/`eprintln!` are unaffected. Use a debug build (`cargo build`, no `--release`)
    // when you need the log output; `--release` is fine (faster) once you trust a config and
    // just want the final `LENS1_RESULT` line.
    env_logger::init();
    log::info!("lens1-vt-sim: Lens 1 deterministic virtual-time streaming sim");

    let fixture = std::env::var("LENS1_FIXTURE").unwrap_or_else(|_| "cordae".to_string());
    let song_path = std::env::var("LENS1_SONG").unwrap_or_else(|_| "SONGS/Cordae.XML".to_string());
    let target_blocks: u64 = std::env::var("LENS1_BLOCKS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(2000);
    let throughput_bps: u32 = std::env::var("LENS1_THROUGHPUT_BPS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(20_000_000);
    let overhead_us: u32 = std::env::var("LENS1_OVERHEAD_US")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(500);
    let audio_block_us: u64 = std::env::var("LENS1_AUDIO_BLOCK_US")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(DEFAULT_AUDIO_BLOCK_PERIOD_US);
    // Bound on VIRTUAL time simulated before giving up (NOT wall time — see
    // `drive`'s doc comment): generous, since a wedged scenario should fail
    // loudly rather than "simulate" forever.
    let budget_ms: u64 = std::env::var("LENS1_VIRTUAL_BUDGET_MS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(120_000);
    let step_timeout_s: u64 = std::env::var("LENS1_STEP_TIMEOUT_S")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(30);

    log::info!(
        "lens1-vt-sim: fixture={fixture} song={song_path} target_blocks={target_blocks} \
         throughput_bps={throughput_bps} overhead_us={overhead_us} audio_block_us={audio_block_us} \
         budget_ms={budget_ms}"
    );

    // Pack (or reuse) a real FAT SD image from the golden corpus — same tooling
    // Task 5's scenario driver uses. Must run before anything below touches SD.
    if std::env::var_os("DELUGE_SD_IMAGE").is_none() {
        let repo_root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(4)
            .expect("CARGO_MANIFEST_DIR (src/bsp/rust/lens1_vt_sim) has a repo root 4 levels up")
            .to_path_buf();
        let img = sd_image::pack_golden_fixture(&repo_root, &fixture);
        // SAFETY: single-threaded at this point (before any task/executor exists).
        unsafe { std::env::set_var("DELUGE_SD_IMAGE", &img) };
    }

    // See this file's module doc: sidesteps the boot-time off-fiber block_on
    // livelock. Set once, before anything spawns.
    sd::sim_latency::set_off_fiber_instant(true);
    // Zero-jitter starvation guard (see `fiber.rs`'s `HIGH_PRIORITY_FAIRNESS_BOUND`
    // doc comment): without this, `loader::request_pump`'s HIGH-priority
    // re-enqueue (~100-200us cadence, no wall-clock jitter to break the tie)
    // starves song-load's NORMAL-priority dispatch forever on this virtual
    // clock. 8 is a small, arbitrary bound — large enough that a genuinely
    // urgent HIGH burst still wins comfortably, small enough that a starved
    // NORMAL job waits at most ~8 HIGH cycles (under 2ms of virtual time).
    fiber::set_high_priority_fairness_bound(8);
    // Task 6's `post_load_sim_latency` design applies the modeled latency ONLY
    // after load completes, deliberately sparing the essential-sample preload
    // (load has no real-time deadline). That's also WHY it can never produce a
    // WAIT-class underrun on its own: `SampleHolder::claimClusterReasons`
    // (called during load, `CLUSTER_ENQUEUE`) always leaves `clusters[0]`
    // loaded by the time playback starts (see `LoadSongUI::performLoad`'s
    // internal `yieldWithTimeout(..., 5)` gate at load_song_ui.cpp:480, which
    // waits — at the DEFAULT fast latency — comfortably within 5s). Setting
    // `LENS1_PRELOAD_LATENCY=1` instead applies the SAME throughput/overhead
    // globally, from before boot, so essential preload is ALSO starved: if a
    // single cluster fetch takes longer than that internal 5s gate, load
    // proceeds anyway with `clusters[0]->loaded == false`, and
    // `attemptLateSampleStart`'s WAIT branch should fire the instant
    // `resumePlayback`/note-on checks it. This is the harness's positive-
    // control lever (Task 7's GATE) — see the report for evidence either way.
    let preload_latency = std::env::var("LENS1_PRELOAD_LATENCY")
        .ok()
        .is_some_and(|s| s == "1");
    #[cfg(feature = "sim_latency")]
    if preload_latency {
        sd::sim_latency::set_throughput_bytes_per_sec(throughput_bps);
        sd::sim_latency::set_command_overhead_us(overhead_us);
    }
    // Applied to `scenario::run` AFTER load completes (see
    // `ScenarioConfig::post_load_sim_latency`'s doc comment) — this is the
    // knob that stresses sustained real-time streaming, not load itself. When
    // `preload_latency` is set the override is already globally in effect, so
    // there's nothing left for `scenario::run` to apply post-load.
    let post_load_sim_latency = if preload_latency {
        None
    } else {
        Some((throughput_bps, overhead_us))
    };

    // Pin the audio (priority-0) task to the exact virtual block cadence — see
    // `scheduler.rs`'s `AUDIO_PERIOD_OVERRIDE_US` doc comment (the brief's
    // "compute-budget" knob).
    scheduler::set_audio_period_override_us(audio_block_us, 0);

    // SAFETY: `raw::Executor` needs `&'static`; leaking a Box is the standard
    // pattern for a `fn main` that doesn't itself run forever (Spike A /
    // `embassy_executor::Executor::run`'s own doc use the same pattern).
    let executor: &'static raw::Executor =
        Box::leak(Box::new(raw::Executor::new(std::ptr::null_mut())));
    let spawner = executor.spawner();
    scheduler::set_spawner(spawner);

    // Same BSP task set `deluge-bsp-rust`'s host_app boot block spawns (minus
    // the audio-thread split — see module doc: with `scheduler::set_audio_spawner`
    // never called, the priority-0 audio task falls back to this single spawner
    // automatically, the documented fallback path in `scheduler.rs`'s `claim`).
    spawner.spawn(control::pic_pump().unwrap());
    spawner.spawn(control::pad_render().unwrap());
    spawner.spawn(control::encoder_wake_pump().unwrap());
    spawner.spawn(display::oled_render().unwrap());
    spawner.spawn(sd::sim_latency::pump().unwrap());
    spawner.spawn(boot_task().unwrap());

    let song_path_static: &'static str = Box::leak(song_path.into_boxed_str());
    let cfg = scenario::ScenarioConfig {
        song_full_path: song_path_static,
        target_blocks,
        step_timeout: Duration::from_secs(step_timeout_s),
        post_load_sim_latency,
    };
    static DONE: AtomicBool = AtomicBool::new(false);
    spawner.spawn(scenario_runner(cfg, &DONE).unwrap());

    // --- Spike A's discrete-event driver loop -------------------------------
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
                "lens1-vt-sim: scenario task completed at virtual t={}us ({quiescence_passes} \
                 quiescence passes, {advances} clock advances)",
                driver.now(),
            );
            break;
        }

        let Some(next) = driver.peek_next_deadline() else {
            log::error!(
                "lens1-vt-sim: no task has an outstanding timer — scenario wedged (deadlock)"
            );
            hard_exit(2);
        };
        if next > budget_ticks {
            log::error!(
                "lens1-vt-sim: virtual-time budget ({budget_ms}ms) exhausted before the scenario \
                 completed (next deadline at {next}us > budget {budget_ticks}us) — scenario wedged"
            );
            hard_exit(2);
        }
        let now = driver.now();
        driver.advance_ticks(next - now);
        advances += 1;
        if advances.is_multiple_of(20_000) {
            log::info!(
                "lens1-vt-sim: progress: virtual t={}us advances={advances} passes={quiescence_passes} \
                 on_fiber_reads={} on_fiber_writes={}",
                driver.now(),
                sd::stats::on_fiber_reads(),
                sd::stats::on_fiber_writes(),
            );
        }
    }
    let wall_elapsed = wall_start.elapsed();
    log::info!("lens1-vt-sim: wall-clock elapsed: {wall_elapsed:?}");

    let result = RESULT.lock().unwrap().take().expect("scenario result");
    let lifetime_wait = unsafe { deluge_sim_underrun_wait_count() };
    let lifetime_unassign = unsafe { deluge_sim_underrun_unassign_count() };
    log::info!(
        "lens1-vt-sim: FINAL underrun_wait={} underrun_unassign={} (lifetime totals: \
         wait={lifetime_wait} unassign={lifetime_unassign})",
        result.underrun_wait,
        result.underrun_unassign,
    );

    let ok = result.load_completed && result.playback_confirmed_active;
    if !ok {
        log::error!("lens1-vt-sim: scenario did not reach sustained playback (see fields above)");
        hard_exit(1);
    }

    // Single machine-parseable summary line for shell-script harnesses.
    println!(
        "LENS1_RESULT blocks_rendered={} cluster_reads={} recorder_writes={} underrun_wait={} \
         underrun_unassign={}",
        result.blocks_rendered,
        result.cluster_reads,
        result.recorder_writes,
        result.underrun_wait,
        result.underrun_unassign,
    );
    hard_exit(0);
}

/// Immediate, unconditional process termination — skips libc's atexit-run C++
/// static destructors. Same rationale (and same raw `exit_group` shape) as
/// `../src/main.rs`'s `hard_exit`: the real C++ app links objects with static
/// storage duration (`AudioEngine`, `playbackHandler`, …) that register
/// `__cxa_atexit` destructors at process start. A normal `std::process::exit`
/// still runs those destructors — observed here as a SIGSEGV (exit code 139)
/// AFTER the scenario result was already logged/printed, i.e. destructor
/// ordering against some other still-referenced global, not a bug in the
/// scenario itself. This process never needs an orderly C++ shutdown (there
/// is no real device shutdown path either), so skip it.
///
/// Flushes stdout/stderr first: `log`/`env_logger` (via its `anstream` dependency)
/// buffers writes rather than flushing per line, so without this, any run that
/// fails fast — a wedged/timed-out scenario detected only a few log lines into
/// `main`, before enough volume accumulates to trigger an internal flush — would
/// otherwise exit with its diagnostic log output silently discarded (observed
/// directly while investigating this harness: a failing run produced a bare
/// `exit_group(1)` with ZERO captured bytes on either stream). This flush is why
/// the fast-fail case still reports its cause.
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
