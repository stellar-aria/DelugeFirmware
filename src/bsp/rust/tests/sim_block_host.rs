//! Host tests for the `sim_block` seam (R5a Phase 0). Verifies that a future which is
//! Pending until an external agent acts can still complete under a non-yielding block,
//! provided a progress hook is registered — and that an unsatisfiable future trips the
//! wedge budget instead of spinning forever.
#![cfg(not(target_os = "none"))]

use core::future::Future;
use core::pin::Pin;
use core::sync::atomic::{AtomicU32, Ordering};
use core::task::{Context, Poll};

// No `deluge_bsp_rust::` path exists (bin-only crate) — re-include the module the same way
// `tests/owner_host.rs:15-18` re-includes `fiber`/`sd`.
#[path = "../src/sim_block.rs"]
mod sim_block;

use sim_block::Progress;

/// Ready only once `TICKS` has been bumped `n` times by the progress hook.
struct NeedsTicks {
    needed: u32,
}
static TICKS: AtomicU32 = AtomicU32::new(0);

impl Future for NeedsTicks {
    type Output = u32;
    fn poll(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<u32> {
        let seen = TICKS.load(Ordering::SeqCst);
        if seen >= self.needed {
            Poll::Ready(seen)
        } else {
            Poll::Pending
        }
    }
}

fn bump_hook() -> Progress {
    TICKS.fetch_add(1, Ordering::SeqCst);
    Progress::Advanced
}

#[test]
fn block_on_completes_when_hook_makes_progress() {
    TICKS.store(0, Ordering::SeqCst);
    sim_block::set_progress_hook(bump_hook);
    sim_block::set_spin_budget(1000);

    let got = sim_block::block_on(NeedsTicks { needed: 5 });

    assert_eq!(
        got, 5,
        "future should complete once the hook has ticked enough"
    );
}

fn stalled_hook() -> Progress {
    Progress::Stalled
}

#[test]
#[should_panic(expected = "sim_block: wedged")]
fn block_on_wedges_instead_of_spinning_forever() {
    TICKS.store(0, Ordering::SeqCst);
    sim_block::set_progress_hook(stalled_hook);
    sim_block::set_spin_budget(8);

    // Needs ticks the stalled hook will never supply.
    let _ = sim_block::block_on(NeedsTicks { needed: u32::MAX });
}
