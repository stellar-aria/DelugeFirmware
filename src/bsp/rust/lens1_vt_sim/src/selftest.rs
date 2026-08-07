//! Phase 0's proof: a modeled SD read, driven by a NON-YIELDING `sim_block::block_on`,
//! completes — because the progress hook pumps `HP_EXEC` (where `sim_latency::pump` lives)
//! and advances the virtual clock.
//!
//! Before Phase 0 this shape was the documented livelock: `embassy_futures::block_on` over
//! `sim_latency::modeled_read` spun at 100% CPU with the virtual clock frozen, because
//! nothing could poll `pump` (see `sd.rs`'s `off_fiber_instant` doc comment).
//!
//! # Why `main` pumps `HP_EXEC` once before issuing the transfer
//!
//! `main` calls `preempt::pump_hp()` before [`block_on_modeled_read`] so that
//! `sim_latency::pump`'s `REQUEST.wait()` is registered ahead of the first transfer. This
//! matters less than it sounds: `REQUEST` is an `embassy_sync::signal::Signal`, which
//! LATCHES — it stores `Signaled` regardless of whether anything is waiting yet, and a
//! later `poll_wait` sees that latched state and returns `Ready` immediately. `pump` is
//! also already sitting in `HP_EXEC`'s run queue the moment it's spawned. So skipping the
//! pre-pump would not lose the signal; it would only cost one extra `sim_block::block_on`
//! spin iteration (the first poll of `locked_read_sectors` would find `pump` not yet
//! polled, get `Pending`, and the progress hook's own `pump_hp()` would pick it up on the
//! very next iteration). It's still correct discipline to pump first — it makes the
//! ordering explicit rather than relying on `Signal`'s latching behavior, and it is the
//! right call if `REQUEST` is ever swapped for something non-latching.
use embassy_time::Instant;

use crate::{sd, sim_block};

/// Read one sector under a non-yielding block and assert that the modeled read took
/// EXACTLY the virtual latency `sim_latency::latency_for` predicts for that transfer —
/// not merely a non-zero amount. An exact comparison (rather than a `!= 0` check) proves
/// the completion came from the modeled path specifically, not from some other timer
/// firing to end the spin; it also stays correct when the model itself predicts zero
/// (e.g. `LENS1_OVERHEAD_US=0` with a high enough throughput override), where a `!= 0`
/// check would misreport a genuine pass as "the latency model was skipped".
pub fn block_on_modeled_read() -> Result<(), String> {
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
    log::info!(
        "selftest: non-yielding block_on over a modeled read completed in {}us virtual time \
         (matches sim_latency::latency_for({}) exactly)",
        elapsed.as_micros(),
        buf.len(),
    );
    Ok(())
}
