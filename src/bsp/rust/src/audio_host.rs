//! `audio_io.h` — host null-sink render pump (`host_app` feature, M4c Task 1).
//!
//! On device (`audio.rs`) `deluge_audio_drive` trickles the app's render into
//! the SSI TX DMA ring, paced by the free-running play head. There is no DMA
//! (or any audio device at all) on host: this module instead makes the
//! priority-0 C++ task (`AudioEngine::routine_task`, spawned by
//! `registerTasks()` during `deluge_app_init` — see `scheduler.rs`'s
//! `addRepeatingTask`) actually call `deluge_app_render` each time it drives,
//! discarding the output block (a null sink), so the render graph exercises
//! for real instead of M4b's no-op stub in `host_link_stubs.rs`.
//!
//! M4c Task 2 adds a second, preemptive host executor thread (`"deluge-audio"`,
//! spawned in `main.rs`'s `host_app` boot path) that `scheduler::set_audio_spawner`
//! routes the priority-0 task onto — the host analogue of device `audio.rs`'s
//! `AUDIO_EXEC` SGI executor. `AudioEngine::runRoutine()` (audio_engine.cpp) also
//! calls `routine()` — and so `deluge_audio_drive` — directly and synchronously
//! from `deluge_boot()`, before `registerTasks()` assigns `routine_task_id`
//! ("necessary otherwise Deluge freezes on boot"); that one pre-registration call
//! runs on whichever thread called `deluge_boot` (the host-app executor thread),
//! not `"deluge-audio"` — expected, and `deluge_audio_drive` below accounts for
//! it rather than treating it as a routing failure. Once `registerTasks()`
//! returns, every subsequent call is driven by the scheduled task and runs on
//! `"deluge-audio"`. No ISR and no other task touches this module's state
//! concurrently with a render *from the same thread*, but the render itself can
//! now run concurrently with everything else (races enumerated under TSan in
//! Task 3) — the `static mut` buffer access is no longer single-threaded in the
//! way `audio.rs` documents for its own statics; that's this task's whole point.
#![allow(non_upper_case_globals)]

use core::ptr::{addr_of, addr_of_mut};
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use crate::sys::DelugeStereoSample;

unsafe extern "C" {
    /// The app renders `frames` stereo samples into `output`, reading `frames`
    /// aligned input samples from `input` (app.h). i32-native; no scaling
    /// here. Exact mirror of device `audio.rs`'s import (same signature, same
    /// ABI) — the host-built `deluge_app` object closure exports the same
    /// symbol.
    fn deluge_app_render(
        input: *const DelugeStereoSample,
        output: *mut DelugeStereoSample,
        frames: u32,
    );
}

/// Maximum frames per `deluge_app_render` call. MUST match device `audio.rs`'s
/// `APP_BLOCK_FRAMES` (128, not `host_audio.c`'s 32): the app's internal
/// render-buffer globals (`renderingMemory`/`reverbMemory`) are fixed at this
/// size (board_config.h `SSI_TX_BUFFER_NUM_SAMPLES`), and a larger block
/// makes the app write out of bounds into adjacent globals. This is a real
/// ABI constraint of the linked C++ object, not a host-only pacing choice —
/// so it stays in lock-step with the device value regardless of host pacing.
const APP_BLOCK_FRAMES: usize = 128;

const ZERO: DelugeStereoSample = DelugeStereoSample { l: 0, r: 0 };

// Null-sink render scratch. Plain host statics (no SDRAM section on host —
// that's a device linker-script concept); the app renders into RENDER_BLOCK
// and reads INPUT_BLOCK (silence), and both are discarded/left as-is —
// nothing downstream ever reads them.
static mut RENDER_BLOCK: [DelugeStereoSample; APP_BLOCK_FRAMES] = [ZERO; APP_BLOCK_FRAMES];
static mut INPUT_BLOCK: [DelugeStereoSample; APP_BLOCK_FRAMES] = [ZERO; APP_BLOCK_FRAMES];

/// Monotonic host frame cursor: total frames rendered so far, advanced by
/// `APP_BLOCK_FRAMES` on every `deluge_audio_drive` call. Stands in for the
/// device's DMA play head (`tx_play_frame()` in `audio.rs`) — there is no
/// real playback device on host, so "now" is just "how much has been
/// rendered", which is exactly what `deluge_audio_frames_until_block_offset`
/// needs for the (non-sample-accurate) MIDI-gate timing queries the null-sink
/// harness exercises.
static CURSOR: AtomicU64 = AtomicU64::new(0);
/// Cursor value at the start of the most recent render block (the host
/// stand-in for `audio.rs`'s `BLOCK_START_WRITE`/`BLOCK_START_PLAY` pair — on
/// host both collapse to the same value since there's no separate write vs.
/// play head).
static BLOCK_START: AtomicU64 = AtomicU64::new(0);

/// Count of `deluge_audio_drive` calls, for the periodic render-progress log.
static DRIVE_COUNT: AtomicU64 = AtomicU64::new(0);
/// Log a render-progress line every this many drives (plus always on the
/// first), so `deluge_app_render` actually firing is observable without
/// flooding the log at ~44100/128 Hz.
const LOG_EVERY: u64 = 500;

/// M4c Task 2 proof: set once a render is observed running on the
/// `"deluge-audio"` executor thread, i.e. the priority-0 task genuinely routed
/// through `scheduler::set_audio_spawner` rather than staying on whichever
/// thread happened to call `deluge_app_render` (see this module's doc comment
/// re: the pre-registration synchronous call from `deluge_boot`). Polled by the
/// `host_app` boot smoke in `main.rs` before it lets the process exit.
static AUDIO_THREAD_RENDER_SEEN: AtomicBool = AtomicBool::new(false);

/// `true` once a `deluge_audio_drive` call has been observed on the
/// `"deluge-audio"` thread — the host-side evidence that the priority-0 task
/// routed to the second (audio) executor.
pub fn audio_thread_render_seen() -> bool {
    AUDIO_THREAD_RENDER_SEEN.load(Ordering::Relaxed)
}

#[unsafe(no_mangle)]
pub extern "C" fn deluge_audio_max_block_frames() -> u32 {
    APP_BLOCK_FRAMES as u32
}

#[unsafe(no_mangle)]
pub extern "C" fn deluge_audio_sample_rate() -> u32 {
    44_100
}

/// Render one block via the real app render path and discard the output (null
/// sink). Called directly by the priority-0 C++ task (`AudioEngine::routine_task`)
/// — see this module's doc comment; there is no separate pump task here.
///
/// Per the `audio_io.h` contract, the return value is the number of times
/// `deluge_app_render` was invoked this call (0 = no new audio needed), NOT a
/// frame count — mirrors device `audio.rs` and the C host backend
/// `host_audio.c`. This null-sink pump always renders exactly one block per
/// call, so it always returns 1.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_audio_drive() -> u32 {
    let block_start = CURSOR.load(Ordering::Relaxed);
    BLOCK_START.store(block_start, Ordering::Relaxed);

    // SAFETY: single-threaded (see module doc) — no other task or ISR touches
    // RENDER_BLOCK/INPUT_BLOCK. Pointers cover exactly APP_BLOCK_FRAMES
    // elements of the app's native stereo-sample type, matching the device
    // `audio.rs` call site's ABI exactly.
    unsafe {
        deluge_app_render(
            addr_of!(INPUT_BLOCK) as *const DelugeStereoSample,
            addr_of_mut!(RENDER_BLOCK) as *mut DelugeStereoSample,
            APP_BLOCK_FRAMES as u32,
        );
    }
    // Output intentionally discarded: this is a null sink (no DAC/DMA on host).

    CURSOR.store(block_start + APP_BLOCK_FRAMES as u64, Ordering::Relaxed);

    let n = DRIVE_COUNT.fetch_add(1, Ordering::Relaxed) + 1;
    let thread_name = std::thread::current()
        .name()
        .unwrap_or("<unnamed>")
        .to_string();
    if n == 1 {
        // Expected to read the host-app executor thread, NOT "deluge-audio" —
        // this is `AudioEngine::runRoutine()`'s pre-registration direct call
        // (routine_task_id is still -1), made before `registerTasks()` has had
        // a chance to route anything anywhere. See this module's doc comment.
        log::info!("audio: first (pre-registration) render on thread {thread_name:?}");
    }
    if thread_name == "deluge-audio" && !AUDIO_THREAD_RENDER_SEEN.swap(true, Ordering::Relaxed) {
        // M4c Task 2 proof: the priority-0 task's *scheduled* render (not the
        // pre-registration one above) genuinely routed to the second executor
        // thread via `scheduler::set_audio_spawner`, and is running there
        // concurrently with the main/host-app executor thread.
        log::info!("audio: render #{n} routed onto the \"deluge-audio\" executor thread");
    }
    if n == 1 || n.is_multiple_of(LOG_EVERY) {
        log::info!("audio: rendered {n} blocks (last on thread {thread_name:?})");
    }

    1
}

#[unsafe(no_mangle)]
pub extern "C" fn deluge_audio_frames_until_block_offset(
    offset_frames: u32,
    elapsed_frames_out: *mut u32,
) -> u32 {
    // Host stand-in for device `audio.rs`'s TX-ring play-head math: "now" is
    // the monotonic render cursor (no real DMA play head to poll), and there
    // is no ring to mask against, so this is plain (non-panicking) wrapping
    // arithmetic rather than a masked ring offset. Simplified/monotonic per
    // this task's brief — the null-sink harness doesn't need sample-accurate
    // MIDI-gate timing, only a sane, non-panicking answer.
    let play_now = CURSOR.load(Ordering::Relaxed);
    let block_start = BLOCK_START.load(Ordering::Relaxed);

    if !elapsed_frames_out.is_null() {
        // SAFETY: caller-provided out param, matching device `audio.rs`'s
        // contract (non-null implies writable).
        unsafe {
            *elapsed_frames_out = play_now.wrapping_sub(block_start) as u32;
        }
    }

    block_start
        .wrapping_add(offset_frames as u64)
        .wrapping_sub(play_now) as u32
}
