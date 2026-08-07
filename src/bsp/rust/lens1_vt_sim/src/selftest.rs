//! Phase 0's proof: a modeled SD read, driven by a NON-YIELDING `sim_block::block_on`,
//! completes — because the progress hook pumps `HP_EXEC` (where `sim_latency::pump` lives)
//! and advances the virtual clock.
//!
//! Before Phase 0 this shape was the documented livelock: `embassy_futures::block_on` over
//! `sim_latency::modeled_read` spun at 100% CPU with the virtual clock frozen, because
//! nothing could poll `pump` (see `sd.rs`'s `off_fiber_instant` doc comment).
//!
//! Two modes share the identical read-and-assert body ([`read_and_assert_modeled_latency`]),
//! so the ONLY variable between them is the calling context:
//!
//! - [`block_on_modeled_read`] is called directly from `main`, with no `executor.poll()` on
//!   the stack — Task 3's proof.
//! - [`block_on_modeled_read_nested`] is called from INSIDE a task while MAIN's
//!   `executor.poll()` is already on the stack — the shape every Phase 2 production call
//!   site actually uses, and also the shape of the original boot livelock
//!   (`deluge_app_init` → `boot_task` → `executor.poll()`; see `sd.rs:211-231`). Two
//!   mechanisms differ here and could break it: `progress_hook` advancing the virtual clock
//!   while a MAIN task's future is `&mut`-borrowed by the outer `executor.poll()`, and
//!   `clock.rs`'s `peek_next_deadline` popping-and-waking every due timer away from the
//!   quiescence point its own doc comment says it needs. See `main.rs`'s
//!   `--selftest-block-nested` mode for how this gets onto that stack.
//!
//! # Why `main` pumps `HP_EXEC` once before issuing the transfer
//!
//! `main` calls `preempt::pump_hp()` before either mode above so that `sim_latency::pump`'s
//! `REQUEST.wait()` is registered ahead of the first transfer. This matters less than it
//! sounds: `REQUEST` is an `embassy_sync::signal::Signal`, which LATCHES — it stores
//! `Signaled` regardless of whether anything is waiting yet, and a later `poll_wait` sees
//! that latched state and returns `Ready` immediately. `pump` is also already sitting in
//! `HP_EXEC`'s run queue the moment it's spawned. So skipping the pre-pump would not lose
//! the signal; it would only cost one extra `sim_block::block_on` spin iteration (the first
//! poll of `locked_read_sectors` would find `pump` not yet polled, get `Pending`, and the
//! progress hook's own `pump_hp()` would pick it up on the very next iteration). It's still
//! correct discipline to pump first — it makes the ordering explicit rather than relying on
//! `Signal`'s latching behavior, and it is the right call if `REQUEST` is ever swapped for
//! something non-latching. The nested mode follows the same discipline (see its
//! `--selftest-block-nested` call site in `main.rs`): `main` pumps `HP_EXEC` once before
//! spawning the nested task, for the identical reason.
use embassy_time::{Duration, Instant};

use crate::{sd, sim_block};

/// Read one sector under a non-yielding block and assert that the modeled read took
/// EXACTLY the virtual latency `sim_latency::latency_for` predicts for that transfer —
/// not merely a non-zero amount. An exact comparison (rather than a `!= 0` check) proves
/// the completion came from the modeled path specifically, not from some other timer
/// firing to end the spin; it also stays correct when the model itself predicts zero
/// (e.g. `LENS1_OVERHEAD_US=0` with a high enough throughput override), where a `!= 0`
/// check would misreport a genuine pass as "the latency model was skipped".
///
/// Shared by both [`block_on_modeled_read`] and [`block_on_modeled_read_nested`] — see this
/// module's doc comment — so the two modes cannot drift apart on anything but the calling
/// context. Returns `(elapsed, bytes)` rather than just `elapsed` so callers can log the
/// ACTUAL transfer size their `sim_latency::latency_for` comparison was made against,
/// rather than a hardcoded literal that would silently go stale if this function's buffer
/// size ever changed.
fn read_and_assert_modeled_latency() -> Result<(Duration, usize), String> {
    let mut buf = [0u8; 512];
    let expected = sd::sim_latency::latency_for(buf.len());
    let t0 = Instant::now();

    sim_block::block_on(async { sd::locked_read_sectors(0, 1, &mut buf).await })
        .map_err(|e| format!("modeled read failed: {e:?}"))?;

    let elapsed = Instant::now() - t0;
    if elapsed != expected {
        return Err(format!(
            "modeled read completed in {}us virtual time, but `sim_latency::latency_for({})` \
             predicts {}us — the completion did not come from the modeled path this selftest \
             exists to prove. Check that `sim_latency` is enabled and that `off_fiber_instant` \
             is not exempting this path.",
            elapsed.as_micros(),
            buf.len(),
            expected.as_micros(),
        ));
    }
    Ok((elapsed, buf.len()))
}

/// Read one sector under a non-yielding [`sim_block::block_on`] called directly from `main`
/// — no `executor.poll()` on the stack. See this module's doc comment.
pub fn block_on_modeled_read() -> Result<(), String> {
    let (elapsed, bytes) = read_and_assert_modeled_latency()?;
    log::info!(
        "selftest: non-yielding block_on over a modeled read, called directly from `main` \
         (no executor.poll() on the stack), completed in {}us virtual time (matches \
         sim_latency::latency_for({bytes}) exactly)",
        elapsed.as_micros(),
    );
    Ok(())
}

/// Read one sector under a non-yielding [`sim_block::block_on`] called from INSIDE a task
/// while MAIN's `executor.poll()` is already on the stack — the nested sibling of
/// [`block_on_modeled_read`]. See this module's doc comment for why this shape matters and
/// what could break it; see `main.rs`'s `--selftest-block-nested` mode for how the caller
/// gets this call onto that stack (a MAIN task, polled by the driver loop).
pub fn block_on_modeled_read_nested() -> Result<(), String> {
    let (elapsed, bytes) = read_and_assert_modeled_latency()?;
    log::info!(
        "selftest: non-yielding block_on over a modeled read, called from INSIDE a task \
         under MAIN's executor.poll(), completed in {}us virtual time (matches \
         sim_latency::latency_for({bytes}) exactly)",
        elapsed.as_micros(),
    );
    Ok(())
}
