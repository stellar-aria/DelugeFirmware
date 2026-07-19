//! The streaming-underrun harness's REUSABLE scenario driver (Phase 1 shared substrate,
//! Task 5): load a real song, start real-time playback, start a concurrent output
//! recording, then step until a target number of audio blocks have rendered — the
//! substrate both lenses (Task 7's deterministic virtual-time sim, Task 9's preemptive-
//! audio TSan check) assert over.
//!
//! [`run`] is deliberately THREAD-AGNOSTIC: it never spawns an OS thread, never touches
//! [`crate::sd::sim_latency::pump`] (that mechanism is Lens-2-only — Task 7's Lens 1 runs
//! on a single-threaded custom executor and cannot spawn an escape-hatch thread, see
//! `.superpowers/sdd/task-4-report.md`'s cross-lens note), and makes no assumption about
//! which embassy executor is driving it (`platform-std`, Lens 1's future custom
//! `raw::Executor`+`MockDriver`, ...) — it only `.await`s [`embassy_time::Timer`]s in a
//! plain poll loop, which every embassy executor variant supports identically. The caller
//! is responsible for having already booted the real C++ app (`deluge_app_init` returned,
//! and *something* is draining the worker-fiber ring — see `fiber::worker_poll()`) on
//! whatever executor is driving it; this module just issues the same C-ABI calls a
//! LOAD/PLAY/RECORD button press would (`harness/streaming_scenario.h`/`.cpp` on the C++
//! side) and polls the same observable state a UI would (`LoadSongUI::isLoadingSong()`,
//! `PlaybackHandler::isEitherClockActive()`).
//!
//! [`scenario_task`] is a thin, NOT-reusable convenience wrapper used only by this crate's
//! own `host_app` boot path (`main.rs`): it spawns [`run`] as an embassy task and stashes
//! the result in a static so the OS thread that owns that executor (which cannot `.await`
//! directly) can poll for completion. Lens 1/2 harnesses are expected to call [`run`]
//! directly from their own executor's task graph instead of reusing this wrapper.
#![cfg(all(not(target_os = "none"), feature = "host_app"))]

use core::ffi::c_char;
use std::ffi::CString;
use std::sync::Mutex;

use embassy_time::{Duration, Instant, Timer};

// Host-only C-ABI bridge (src/deluge/harness/streaming_scenario.{h,cpp}, `DELUGE_HOST`-
// guarded — compiled into the linked host_app `deluge_app` object closure, never into the
// ARM device firmware). Manually declared here (not through the bindgen `sys` module, same
// pattern as `deluge_app_init` in `main.rs`) since these are harness-only entry points the
// app exposes to the platform, not part of the libdeluge C-ABI the app CONSUMES.
unsafe extern "C" {
    fn deluge_scenario_begin_song_load(full_path: *const c_char) -> bool;
    fn deluge_scenario_song_listing_in_progress() -> bool;
    fn deluge_scenario_commit_song_load() -> bool;
    fn deluge_scenario_song_load_in_progress() -> bool;
    fn deluge_scenario_start_playback();
    fn deluge_scenario_playback_active() -> bool;
    fn deluge_scenario_start_recording() -> bool;
}

/// Parameters for [`run`]. `Copy` so [`scenario_task`] can take it by value (embassy task
/// arguments must be owned).
#[derive(Debug, Clone, Copy)]
pub struct ScenarioConfig {
    /// Song path within the card image, e.g. `"SONGS/Cordae.XML"` (same shape
    /// `Song::setSongFullPath` expects).
    pub song_full_path: &'static str,
    /// How many ADDITIONAL audio blocks to render (beyond the baseline sampled right
    /// before playback starts) before the scenario is considered done.
    pub target_blocks: u64,
    /// Bound on each polling wait (listing / load-commit / playback-start / block target)
    /// so a wedged app fails the scenario instead of hanging the caller forever.
    pub step_timeout: Duration,
}

/// Outcome of [`run`]. Deliberately plain data, not a `Result`/panic: a step that times
/// out just leaves the later fields at their default and `run` returns early — the caller
/// decides what counts as pass/fail (matters for the later lens tasks' NEGATIVE controls,
/// which need to observe "the harness correctly detects failure", not just "it panicked").
#[derive(Debug, Default, Clone, Copy)]
pub struct ScenarioResult {
    /// The app finished enough of boot (`registerTasks()` claimed at least one scheduler
    /// slot — the same signal `main.rs`'s own boot-OK check uses) for `currentSong` to
    /// exist. `run` is safe to call as soon as SOMETHING is draining the worker-fiber ring
    /// (`fiber::worker_poll()`) concurrently — it does not itself require the caller to
    /// have already confirmed boot; this field records that `run` waited it out itself.
    pub boot_ready: bool,
    /// `deluge_scenario_begin_song_load` returned true (currentSong existed to target).
    pub song_load_dispatched: bool,
    /// The async directory listing it triggers completed within `step_timeout`.
    pub listing_completed: bool,
    /// `deluge_scenario_commit_song_load` dispatched onto the storage-owner fiber.
    pub load_committed: bool,
    /// The committed load (`LoadSongUI::performLoad`) finished within `step_timeout`.
    pub load_completed: bool,
    /// `deluge_scenario_start_playback` was called.
    pub playback_started: bool,
    /// Playback was observed active (`PlaybackHandler::isEitherClockActive`) within
    /// `step_timeout` of starting it.
    pub playback_confirmed_active: bool,
    /// `AudioRecorder::beginOutputRecording` returned true.
    pub recording_started: bool,
    /// Audio blocks rendered from just before playback started to when the target was
    /// reached (or the timeout gave up) — see `crate::audio_host::drive_count()`.
    pub blocks_rendered: u64,
    /// On-fiber SD block reads observed over the same window — the loader's real,
    /// dispatched cluster reads (`crate::sd::stats::on_fiber_reads()`). This is the
    /// upgrade over Task 4's proxy: proof `request_pump`'s REAL dispatch, with a REAL
    /// queued cluster (this song's samples), drained through `fiber::worker_poll()`.
    pub cluster_reads: u64,
    /// On-fiber SD block writes observed over the same window — the recorder's real,
    /// dispatched card writes (`crate::sd::stats::on_fiber_writes()`).
    pub recorder_writes: u64,
}

/// Polls `cond` every 5ms (yielding to the executor between polls via
/// [`embassy_time::Timer`] — thread-agnostic, works under any embassy executor) until it
/// returns true or `timeout` elapses. Returns whether `cond` was satisfied.
async fn wait_for(timeout: Duration, mut cond: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + timeout;
    loop {
        if cond() {
            return true;
        }
        if Instant::now() >= deadline {
            return false;
        }
        Timer::after_millis(5).await;
    }
}

/// Runs the scenario against `cfg`. See the module doc for the thread-agnostic contract.
/// Returns as soon as a step fails/times out (with the fields up to that point populated
/// honestly) rather than pressing on into a state the harness can no longer trust.
pub async fn run(cfg: ScenarioConfig) -> ScenarioResult {
    let mut result = ScenarioResult::default();

    // `run` may be spawned onto the same executor as (and concurrently with, from the
    // scheduler's point of view) the task that itself calls `deluge_app_init()` — embassy
    // doesn't guarantee poll order across tasks spawned in the same batch, so wait for
    // boot to actually reach `registerTasks()` (same signal `main.rs`'s own boot-OK check
    // uses) before touching any C++ app state (`currentSong` doesn't exist before then).
    result.boot_ready = wait_for(cfg.step_timeout, || {
        crate::scheduler::registered_task_count() > 0
    })
    .await;
    if !result.boot_ready {
        return result;
    }

    let path = match CString::new(cfg.song_full_path) {
        Ok(p) => p,
        Err(_) => return result, // NUL in the path — caller bug, report honestly rather than UB
    };
    result.song_load_dispatched = unsafe { deluge_scenario_begin_song_load(path.as_ptr()) };
    if !result.song_load_dispatched {
        return result;
    }

    result.listing_completed = wait_for(cfg.step_timeout, || unsafe {
        !deluge_scenario_song_listing_in_progress()
    })
    .await;
    if !result.listing_completed {
        return result;
    }

    result.load_committed = unsafe { deluge_scenario_commit_song_load() };
    if !result.load_committed {
        return result;
    }
    result.load_completed = wait_for(cfg.step_timeout, || unsafe {
        !deluge_scenario_song_load_in_progress()
    })
    .await;
    if !result.load_completed {
        return result;
    }

    // Baseline right before starting playback/recording — everything counted from here on
    // is genuinely attributable to THIS run's streaming/recording, not the song load
    // itself (which also issues on-fiber SD reads for its essential-sample clusters).
    let blocks_before = crate::audio_host::drive_count();
    let reads_before = crate::sd::stats::on_fiber_reads();
    let writes_before = crate::sd::stats::on_fiber_writes();

    unsafe { deluge_scenario_start_playback() };
    result.playback_started = true;
    result.playback_confirmed_active = wait_for(cfg.step_timeout, || unsafe {
        deluge_scenario_playback_active()
    })
    .await;

    result.recording_started = unsafe { deluge_scenario_start_recording() };

    // Step until the target block count or the timeout — either way, report exactly what
    // happened (the caller decides whether a short-of-target count is a failure).
    wait_for(cfg.step_timeout, || {
        crate::audio_host::drive_count().saturating_sub(blocks_before) >= cfg.target_blocks
    })
    .await;

    result.blocks_rendered = crate::audio_host::drive_count().saturating_sub(blocks_before);
    result.cluster_reads = crate::sd::stats::on_fiber_reads().saturating_sub(reads_before);
    result.recorder_writes = crate::sd::stats::on_fiber_writes().saturating_sub(writes_before);

    result
}

/// Cross-thread handoff for [`scenario_task`] — see its doc comment.
static RESULT: Mutex<Option<ScenarioResult>> = Mutex::new(None);

/// NOT part of the reusable driver (see module doc) — a convenience wrapper for THIS
/// crate's own `host_app` boot path (`main.rs`), which owns the host-app executor from a
/// plain OS thread and can't `.await` [`run`] directly. Spawn this as an embassy task on
/// the SAME executor `host_app_task`'s worker-fiber pump loop runs on, then poll
/// [`take_result`] from the OS thread.
#[embassy_executor::task]
pub async fn scenario_task(cfg: ScenarioConfig) {
    let result = run(cfg).await;
    *RESULT.lock().unwrap() = Some(result);
}

/// Takes the result [`scenario_task`] stashed, if it has finished. `None` before
/// completion.
pub fn take_result() -> Option<ScenarioResult> {
    RESULT.lock().unwrap().take()
}
