//! Emulated interrupt preemption for the single-threaded Lens 1 sim (R5a Phase 0).
//!
//! On device, a non-yielding `block_on` in thread mode is preempted by an interrupt
//! executor, so the task holding a lock (or the `sim_latency::pump` equivalent) still runs
//! and the spin completes. Host has one OS thread and no interrupts, so this module
//! emulates that: the spin's progress hook polls a SECOND executor (`HP_EXEC`) and then
//! advances the virtual clock.
//!
//! Polling `HP_EXEC` from inside a task running on the MAIN executor is sound: they are
//! distinct `raw::Executor` instances with distinct state, the process is single-threaded,
//! and this is precisely what `embassy_executor::InterruptExecutor::on_interrupt()` does on
//! device (poll executor B while executor A's `poll()` sits on the stack). It is NOT the
//! reentrant same-executor poll that `raw::Executor::poll` forbids.
use core::sync::atomic::{AtomicBool, Ordering};

use embassy_executor::raw;
// Brings `PeekableMockDriver::now()` (the `embassy_time_driver::Driver` trait method used
// by `progress_hook`, below) into scope; `peek_next_deadline`/`advance_ticks` are inherent
// methods and need no import.
use embassy_time_driver::Driver as _;

use crate::clock;
use crate::sim_block::Progress;

/// Pender context tags. `__pender` receives the context pointer the executor was created
/// with, so distinct tags let one global pender route to the right flag.
pub const MAIN_TAG: usize = 1;
pub const HP_TAG: usize = 2;

static MAIN_PENDED: AtomicBool = AtomicBool::new(false);
static HP_PENDED: AtomicBool = AtomicBool::new(false);

static mut HP: Option<&'static raw::Executor> = None;

/// Record a pend against the executor identified by `context`.
pub fn note_pend(context: *mut ()) {
    if context as usize == HP_TAG {
        HP_PENDED.store(true, Ordering::SeqCst);
    } else {
        MAIN_PENDED.store(true, Ordering::SeqCst);
    }
}

pub fn main_pended() -> bool {
    MAIN_PENDED.load(Ordering::SeqCst)
}

pub fn clear_main_pended() {
    MAIN_PENDED.store(false, Ordering::SeqCst);
}

pub fn hp_pended() -> bool {
    HP_PENDED.load(Ordering::SeqCst)
}

/// Stash `HP_EXEC` and install the `sim_block` progress hook. Call once at start-up,
/// before any task is spawned.
pub fn init(hp: &'static raw::Executor) {
    // SAFETY: single-threaded; called once at start-up before anything is spawned or polled.
    unsafe { *core::ptr::addr_of_mut!(HP) = Some(hp) };
    crate::sim_block::set_progress_hook(progress_hook);
    // Generous but finite: a modeled transfer needs only a handful of pump/advance rounds,
    // so anything near this bound means genuinely wedged, not merely slow.
    crate::sim_block::set_spin_budget(10_000);
}

/// Poll `HP_EXEC` to quiescence. Returns whether it made any pend (i.e. did work).
pub fn pump_hp() -> bool {
    // SAFETY: see `init` — set once before use, read only on the single thread.
    let Some(hp) = (unsafe { *core::ptr::addr_of!(HP) }) else {
        return false;
    };
    let mut did_work = false;
    loop {
        HP_PENDED.store(false, Ordering::SeqCst);
        // SAFETY: HP_EXEC is never polled from within itself — only from the driver loop
        // and from `progress_hook`, which runs inside a MAIN-executor task. See module doc.
        unsafe { hp.poll() };
        if !HP_PENDED.load(Ordering::SeqCst) {
            break;
        }
        did_work = true;
    }
    did_work
}

/// The `sim_block` progress hook: give the higher-priority executor a chance to run, and
/// if it had nothing to do, advance virtual time to the next scheduled deadline.
pub fn progress_hook() -> Progress {
    if pump_hp() {
        return Progress::Advanced;
    }
    let driver = clock::PeekableMockDriver::get();
    match driver.peek_next_deadline() {
        Some(next) => {
            let now = driver.now();
            if next > now {
                driver.advance_ticks(next - now);
            }
            // Let the freshly-woken HP tasks (e.g. `sim_latency::pump`) actually run.
            pump_hp();
            Progress::Advanced
        }
        // No pending timer anywhere and HP is idle: nothing can make the future ready.
        None => Progress::Stalled,
    }
}
