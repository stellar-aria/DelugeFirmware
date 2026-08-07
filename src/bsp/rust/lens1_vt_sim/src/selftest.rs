//! Phase 0's proof: a modeled SD read, driven by a NON-YIELDING `sim_block::block_on`,
//! completes — because the progress hook pumps `HP_EXEC` (where `sim_latency::pump` lives)
//! and advances the virtual clock.
//!
//! Before Phase 0 this shape was the documented livelock: `embassy_futures::block_on` over
//! `sim_latency::modeled_read` spun at 100% CPU with the virtual clock frozen, because
//! nothing could poll `pump` (see `sd.rs`'s `off_fiber_instant` doc comment).
use embassy_time::Instant;

use crate::{sd, sim_block};

/// Read one sector under a non-yielding block and assert that modeled virtual time
/// actually elapsed (proving the latency model ran, rather than being skipped).
pub fn block_on_modeled_read() -> Result<(), String> {
    let mut buf = [0u8; 512];
    let t0 = Instant::now();

    sim_block::block_on(async { sd::locked_read_sectors(0, 1, &mut buf).await })
        .map_err(|e| format!("modeled read failed: {e:?}"))?;

    let elapsed = Instant::now() - t0;
    if elapsed.as_micros() == 0 {
        return Err(
            "modeled read completed in ZERO virtual time — the latency model was skipped, \
             so this proves nothing. Check that `sim_latency` is enabled and that \
             `off_fiber_instant` is not exempting this path."
                .to_string(),
        );
    }
    log::info!(
        "selftest: non-yielding block_on over a modeled read completed in {}us virtual time",
        elapsed.as_micros()
    );
    Ok(())
}
