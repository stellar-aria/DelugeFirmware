//! The shared Owner-dispatch exercise body, driving the REAL `fiber.rs` (the
//! `deluge_worker_run`/`worker_poll` C-ABI `deluge::storage::Owner::run`
//! forwards to — see `src/deluge/storage/owner.cpp`) + `sd.rs` (which owns
//! `deluge_storage_on_owner`) via `crate::fiber`/`crate::sd`, declared by
//! whichever crate root includes this file with `#[path]` — same shape as
//! `tests/support/scheduler_host_exercise.rs`, which this sits alongside.
//!
//! Two crate roots include this file, for the same reason
//! `scheduler_host_exercise.rs`'s header explains (a `cargo test --test`
//! harness under `-Zbuild-std` on a self-hosted sanitizer target hits a
//! reproducible nightly Cargo bug; a plain `fn main()` example binary
//! sidesteps it):
//! - `tests/owner_host.rs` — the normal `cargo test` entry point.
//! - `examples/owner_host_tsan.rs` — the ThreadSanitizer entry point.
//!
//! ## Design note: why submitters go through a channel, not raw threads calling `deluge_worker_run` directly
//!
//! `deluge_worker_run`'s enqueue (`fiber.rs`: `Q_HEAD`/`Q_COUNT`/`QUEUE`) is
//! **not** internally synchronized — plain `static mut`s, no atomics or lock,
//! under a `// SAFETY: single-threaded` comment. This matches its documented
//! contract (`include/libdeluge/worker.h`: "dispatched from a task" — i.e.
//! from *within* the single cooperative Embassy executor thread the whole
//! C++ app runs on; `Cargo.toml` doc comment: "running `deluge_main()` in one
//! Embassy task"). On real hardware this is genuinely never called
//! cross-thread — there is only ever the one executor thread.
//!
//! A throwaway negative control confirmed this empirically: calling
//! `deluge_worker_run` directly from ordinary `std::thread`s (no protection)
//! while `worker_poll`'s `dequeue()` runs concurrently on the host executor
//! thread reliably fires a genuine TSan data race (`fiber.rs:413` `dequeue`
//! vs. `fiber.rs:402` the enqueue write) — see `HOST_HARNESS.md` / the Task 4
//! report for the transcript. This is a real, load-bearing constraint, not
//! just a stale comment: **`deluge_worker_run` (and therefore `Owner::run`)
//! must only ever be called from the executor thread**, same as any other
//! cooperative-task API on this BSP.
//!
//! So "several submitter threads racing to enqueue from a non-executor
//! thread" is modelled the way it would have to work for real — the actual
//! `deluge_worker_run` call happens only on the executor thread (via
//! [`submit_pump`], an Embassy task alongside [`worker_pump`]); genuine
//! non-executor OS threads race to hand off work to it through an
//! `std::sync::mpsc` channel (a primitive that *is* designed for exactly this
//! cross-thread handoff, and — being std, rebuilt under `-Zbuild-std` — is
//! fully visible to TSan's happens-before tracker, unlike an uninstrumented
//! prebuilt-`std` run; see `HOST_HARNESS.md`'s `-Zbuild-std` finding). This
//! keeps the exercise honest: it stresses genuine cross-thread contention
//! (several OS threads racing to get their op accepted) without asking the
//! seam to be something its own contract says it isn't.
//!
//! The **on-owner query** (`deluge_storage_on_owner`, i.e. `fiber::on_fiber`)
//! is a different story — it's a single `AtomicBool` load, safe to call from
//! any thread by construction — so *that* part of the exercise genuinely does
//! read it cross-thread from a non-executor OS thread, no channel needed.

use std::future::Future;
use std::pin::Pin;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::mpsc::{self, TryRecvError};
use std::task::{Context, Poll};
use std::time::{Duration, Instant};

use embassy_executor::{Executor, Spawner};
use embassy_sync::waitqueue::AtomicWaker;

/// A queued submission, handed from a submitter OS thread to [`submit_pump`]
/// over the channel: the op function plus a `usize`-encoded context (avoids a
/// raw-pointer field, which would make the tuple `!Send`).
// (op, thread-id-as-ctx, is_sd_routine, is_high) — is_sd_routine selects
// deluge_worker_run_sd_routine, is_high selects deluge_worker_run_priority
// (mutually exclusive in this exercise — submit_pump checks is_high first);
// neither selects the plain deluge_worker_run.
type Job = (extern "C" fn(*mut core::ffi::c_void), usize, bool, bool);

const NUM_SUBMITTERS: usize = 4;
const OPS_PER_SUBMITTER: u32 = 6;
const TOTAL_FAST_OPS: u32 = NUM_SUBMITTERS as u32 * OPS_PER_SUBMITTER;
/// +1 for the suspending `special_op` (submitted separately, see [`run`]).
const TOTAL_OPS: u32 = TOTAL_FAST_OPS + 1;

// ---------------------------------------------------------------------------
// Shared state the op bodies + driving thread assert over. All plain atomics
// (no locks) — this is exactly the kind of state a broken dispatch (two ops
// overlapping, or the on-owner query lying) would visibly corrupt under TSan
// and under the plain assertions below.
// ---------------------------------------------------------------------------

/// Every op body increments this exactly once, on completion.
static COMPLETED: AtomicU32 = AtomicU32::new(0);

/// One private completion counter per submitter thread, so each thread can
/// throttle itself to at most one outstanding op — keeping the number of
/// jobs actually sitting in `fiber.rs`'s `QUEUE_CAP = 4` ring at or under
/// capacity (`NUM_SUBMITTERS == QUEUE_CAP`), so `deluge_worker_run` never
/// silently drops a submission for being full.
static PER_SUBMITTER_COMPLETED: [AtomicU32; NUM_SUBMITTERS] = [
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
];

/// Serialization contract: `true` while some op body is actively executing on
/// the fiber (from start, or resume-after-yield, until it finishes/next
/// yields). A second op body ever observing this already `true` on entry
/// would prove two ops ran "at once" — i.e. the Owner did NOT serialize them.
static IN_OP: AtomicBool = AtomicBool::new(false);
static SERIALIZATION_VIOLATIONS: AtomicU32 = AtomicU32::new(0);

/// Owner-query contract, the inside-an-op half: every op body checks
/// `deluge_storage_on_owner()` is `true` while it is genuinely executing (an
/// op body only ever runs ON the fiber, so this should never fail).
static ON_OWNER_INSIDE_VIOLATIONS: AtomicU32 = AtomicU32::new(0);

fn fast_op_body(thread_id: usize) {
    if !crate::sd::deluge_storage_on_owner() {
        ON_OWNER_INSIDE_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    if IN_OP.swap(true, Ordering::SeqCst) {
        SERIALIZATION_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    // A little real work under the (believed) exclusion window, widening the
    // interval in which a broken dispatch would actually be caught above.
    std::thread::yield_now();
    IN_OP.store(false, Ordering::SeqCst);
    COMPLETED.fetch_add(1, Ordering::SeqCst);
    PER_SUBMITTER_COMPLETED[thread_id].fetch_add(1, Ordering::SeqCst);
}

/// `extern "C" fn` op body handed to `deluge_worker_run` — the ctx is the
/// submitter's thread id, round-tripped through the C-ABI's `void*`.
extern "C" fn fast_op(ctx: *mut core::ffi::c_void) {
    fast_op_body(ctx as usize);
}

/// The suspending op: like `scheduler_host_exercise.rs`'s `fiber_op`, it
/// yields mid-body (via the real `fiber::yield_until`, the same primitive
/// `scheduler_api.h`'s `yield()` resolves to) and only resumes once
/// [`SPECIAL_GATE`] opens — giving the driving thread a deterministic window
/// in which an op is genuinely in flight (queued/suspended) but NOT
/// literally executing on the fiber, to probe the owner-query's outside half.
static SPECIAL_STARTED: AtomicBool = AtomicBool::new(false);
static SPECIAL_DONE: AtomicBool = AtomicBool::new(false);
static SPECIAL_GATE: AtomicBool = AtomicBool::new(false);
unsafe extern "C" fn special_gate_predicate() -> bool {
    SPECIAL_GATE.load(Ordering::SeqCst)
}

extern "C" fn special_op(_ctx: *mut core::ffi::c_void) {
    if !crate::sd::deluge_storage_on_owner() {
        ON_OWNER_INSIDE_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    if IN_OP.swap(true, Ordering::SeqCst) {
        SERIALIZATION_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    SPECIAL_STARTED.store(true, Ordering::SeqCst);
    // Suspends the fiber; the executor runs worker_pump/submit_pump/everything
    // else until worker_poll's predicate check (SPECIAL_GATE) resumes us.
    crate::fiber::yield_until(Some(special_gate_predicate), None);
    // Resumed: back on the fiber, still the same logical op.
    if !crate::sd::deluge_storage_on_owner() {
        ON_OWNER_INSIDE_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    IN_OP.store(false, Ordering::SeqCst);
    SPECIAL_DONE.store(true, Ordering::SeqCst);
    COMPLETED.fetch_add(1, Ordering::SeqCst);
}

/// An SD-routine-class suspending op (submitted via `deluge_worker_run_sd_routine`,
/// the `is_sd = true` Job). Like `special_op` it yields mid-body and only resumes
/// once [`SD_OP_GATE`] opens, giving the driving thread a window in which the op is
/// genuinely in flight (suspended) to observe that `sd_routine_held()` is engaged.
/// Kept independent of the COMPLETED/IN_OP accounting so it can run as its own
/// phase after the main exercise.
static SD_OP_STARTED: AtomicBool = AtomicBool::new(false);
static SD_OP_DONE: AtomicBool = AtomicBool::new(false);
static SD_OP_GATE: AtomicBool = AtomicBool::new(false);
unsafe extern "C" fn sd_op_gate_predicate() -> bool {
    SD_OP_GATE.load(Ordering::SeqCst)
}
extern "C" fn sd_routine_op(_ctx: *mut core::ffi::c_void) {
    SD_OP_STARTED.store(true, Ordering::SeqCst);
    crate::fiber::yield_until(Some(sd_op_gate_predicate), None);
    SD_OP_DONE.store(true, Ordering::SeqCst);
}

// ---------------------------------------------------------------------------
// Drop-retry contract (rung 4a): `deluge_worker_run` returns false when the
// fixed-capacity ring is full, and once drained a resubmission is accepted
// again. The one-shot UI/smsysex dispatchers rely on that false return to
// release their single-flight guard and retry (LatestWins::reset,
// g_sysex_op_in_flight = false) instead of wedging. Driven as its own phase
// after the main exercise so its burst can't perturb the accounting above.
// ---------------------------------------------------------------------------

/// QUEUE_CAP in fiber.rs is 4 (private there); mirrored here. The invariant this phase asserts is
/// that a burst of CAP+1 *synchronous* enqueues (no executor turn in between, so `worker_pump`
/// cannot drain a slot mid-burst) yields exactly CAP accepted and one refused.
const QUEUE_CAP_MIRROR: usize = 4;
static DROP_TEST_GO: AtomicBool = AtomicBool::new(false);
static DROP_TEST_DONE: AtomicBool = AtomicBool::new(false);
static DROP_ACCEPTED: AtomicU32 = AtomicU32::new(0);
static DROP_DROPPED: AtomicU32 = AtomicU32::new(0);
static DROP_RETRY_OK: AtomicBool = AtomicBool::new(false);

/// A trivial op for the drop test — it only needs to occupy a queue slot; the burst never depends
/// on it doing anything, and it runs harmlessly when the ring later drains.
extern "C" fn drop_test_op(_ctx: *mut core::ffi::c_void) {}

/// The drop-retry phase, run on the executor thread (the only legal caller of `deluge_worker_run`).
/// Bursts CAP+1 enqueues WITHOUT awaiting between them, so `worker_pump` is starved and the ring
/// genuinely fills — the last enqueue must be refused. Then it drains (awaits) and proves a
/// resubmission is accepted again.
#[embassy_executor::task]
async fn drop_test() {
    // Park until the driving thread opens the gate (after the main + sd-routine phases).
    while !DROP_TEST_GO.load(Ordering::SeqCst) {
        embassy_time::Timer::after_millis(2).await;
    }
    // Synchronous burst: no `.await` here, so worker_pump cannot dequeue and the ring fills.
    let mut accepted = 0u32;
    let mut dropped = 0u32;
    for _ in 0..(QUEUE_CAP_MIRROR + 1) {
        if crate::fiber::deluge_worker_run(drop_test_op, core::ptr::null_mut()) {
            accepted += 1;
        } else {
            dropped += 1;
        }
    }
    DROP_ACCEPTED.store(accepted, Ordering::SeqCst);
    DROP_DROPPED.store(dropped, Ordering::SeqCst);
    // Let the executor drain the ring (worker_pump runs the queued ops).
    embassy_time::Timer::after_millis(50).await;
    // Ring drained → a fresh submit is accepted again (the retry path succeeds).
    DROP_RETRY_OK.store(
        crate::fiber::deluge_worker_run(drop_test_op, core::ptr::null_mut()),
        Ordering::SeqCst,
    );
    DROP_TEST_DONE.store(true, Ordering::SeqCst);
}

// ---------------------------------------------------------------------------
// Render-then-complete contract (rung 4b, shape 1): models the async browser
// op's on-fiber UI callback (`folderContentsReady`/`onBrowserOpened`) — it
// mutates shared UI-model state and completes. The property "the executor
// keeps running other tasks while an op is in flight" is already proven above
// by `special_op` (and re-checked by [`run`]'s driving-thread assertion while
// it's parked); this phase reuses that same suspend/resume shape rather than
// re-proving it, and adds the genuinely new assertion: the UI-state write
// happens exactly once, on the fiber, under the same serialization discipline
// as every other op body. Kept on its own statics so it can't perturb the
// COMPLETED/IN_OP accounting the main exercise phase already asserted zero
// violations on.
// ---------------------------------------------------------------------------
static RENDER_STARTED: AtomicBool = AtomicBool::new(false);
static RENDER_DONE: AtomicBool = AtomicBool::new(false);
static RENDER_GATE: AtomicBool = AtomicBool::new(false);
static RENDER_IN_OP: AtomicBool = AtomicBool::new(false);
static RENDER_SERIALIZATION_VIOLATIONS: AtomicU32 = AtomicU32::new(0);
static RENDER_ON_OWNER_VIOLATIONS: AtomicU32 = AtomicU32::new(0);
/// Models the shared UI-model fields an async browser op's completion writes
/// (`folderContentsReady`/`onBrowserOpened`): 0 = not yet rendered, 1 = ready.
static UI_STATE_READY: AtomicU32 = AtomicU32::new(0);
/// Counts how many times the render op body performed the write — must land
/// at exactly 1.
static UI_STATE_WRITES: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn render_gate_predicate() -> bool {
    RENDER_GATE.load(Ordering::SeqCst)
}

extern "C" fn render_op(_ctx: *mut core::ffi::c_void) {
    if !crate::sd::deluge_storage_on_owner() {
        RENDER_ON_OWNER_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    if RENDER_IN_OP.swap(true, Ordering::SeqCst) {
        RENDER_SERIALIZATION_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    RENDER_STARTED.store(true, Ordering::SeqCst);
    // Suspend mid-body — the same mechanism special_op already used to prove
    // the executor keeps running other queued work (worker_pump/submit_pump)
    // while an op is parked; not re-asserted here, just reused, so this op
    // genuinely models "in flight, rendering" rather than a synchronous call.
    crate::fiber::yield_until(Some(render_gate_predicate), None);
    // Resumed: still the same logical op, back on the fiber. This is the
    // "render then complete" write — landing the result into shared
    // UI-model state, same as onBrowserOpened/folderContentsReady would.
    if !crate::sd::deluge_storage_on_owner() {
        RENDER_ON_OWNER_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    UI_STATE_READY.store(1, Ordering::SeqCst);
    UI_STATE_WRITES.fetch_add(1, Ordering::SeqCst);
    RENDER_IN_OP.store(false, Ordering::SeqCst);
    RENDER_DONE.store(true, Ordering::SeqCst);
}

// ---------------------------------------------------------------------------
// Optimistic-open back-out contract (rung 4b, shape 2): models an optimistic
// browser-open that tentatively commits, then discovers the listing failed
// and unwinds (`onListingFailed`) — all inside the same op body, on the
// fiber. Asserts the unwind genuinely runs on the fiber and that the
// "browser open committed" flag is NOT left set once the failed open has
// been unwound. Own statics, independent of every other phase's accounting.
// ---------------------------------------------------------------------------
static BACKOUT_STARTED: AtomicBool = AtomicBool::new(false);
static BACKOUT_DONE: AtomicBool = AtomicBool::new(false);
static BACKOUT_IN_OP: AtomicBool = AtomicBool::new(false);
static BACKOUT_SERIALIZATION_VIOLATIONS: AtomicU32 = AtomicU32::new(0);
static BACKOUT_ON_OWNER_VIOLATIONS: AtomicU32 = AtomicU32::new(0);
static UNWIND_ON_OWNER_VIOLATIONS: AtomicU32 = AtomicU32::new(0);
/// Models "the browser has committed to the newly opened folder" — set
/// optimistically before the listing is confirmed, and expected to be backed
/// out again if the listing then fails.
static OPEN_COMMITTED: AtomicBool = AtomicBool::new(false);
static OPEN_FAILED: AtomicBool = AtomicBool::new(false);
static UNWIND_RAN: AtomicBool = AtomicBool::new(false);

extern "C" fn backout_op(_ctx: *mut core::ffi::c_void) {
    if !crate::sd::deluge_storage_on_owner() {
        BACKOUT_ON_OWNER_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    if BACKOUT_IN_OP.swap(true, Ordering::SeqCst) {
        BACKOUT_SERIALIZATION_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    BACKOUT_STARTED.store(true, Ordering::SeqCst);
    // Optimistic open: commit tentatively, as if the browser had already
    // opened the folder ahead of the listing actually confirming it.
    OPEN_COMMITTED.store(true, Ordering::SeqCst);
    // The listing fails.
    OPEN_FAILED.store(true, Ordering::SeqCst);
    // Unwind (onListingFailed) — same op body, still genuinely on the fiber —
    // backs the optimistic commit back out.
    if !crate::sd::deluge_storage_on_owner() {
        UNWIND_ON_OWNER_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    OPEN_COMMITTED.store(false, Ordering::SeqCst);
    UNWIND_RAN.store(true, Ordering::SeqCst);
    BACKOUT_IN_OP.store(false, Ordering::SeqCst);
    BACKOUT_DONE.store(true, Ordering::SeqCst);
}

// ---------------------------------------------------------------------------
// Two-level priority contract (rung 5 task 2): `deluge_worker_run_priority`
// (HIGH) must dequeue ahead of every `deluge_worker_run`/`_sd_routine`
// (NORMAL) op already sitting in the ring, and FIFO must hold within each
// level. Shape: park a NORMAL op on a gate so the fiber is busy (the ring
// only fills — nothing drains — while it's parked); while parked, enqueue
// four ops in enqueue order NORMAL-A, HIGH-B, HIGH-C, NORMAL-D; release the
// park and assert the *run* order is B, C before A, D (HIGH-before-NORMAL)
// with B before C and A before D (FIFO within each level) — despite A having
// enqueued before B. Own statics, independent of every other phase.
// ---------------------------------------------------------------------------
static PRIO_PARK_STARTED: AtomicBool = AtomicBool::new(false);
static PRIO_PARK_DONE: AtomicBool = AtomicBool::new(false);
static PRIO_PARK_GATE: AtomicBool = AtomicBool::new(false);
unsafe extern "C" fn prio_park_gate_predicate() -> bool {
    PRIO_PARK_GATE.load(Ordering::SeqCst)
}
extern "C" fn prio_park_op(_ctx: *mut core::ffi::c_void) {
    PRIO_PARK_STARTED.store(true, Ordering::SeqCst);
    crate::fiber::yield_until(Some(prio_park_gate_predicate), None);
    PRIO_PARK_DONE.store(true, Ordering::SeqCst);
}

/// Assigns each of the four priority-phase ops the position it actually ran
/// in (0-based, via `fetch_add`), so the driving thread can compare run order
/// after the fact without racing to observe it live.
static PRIO_RUN_ORDER: AtomicU32 = AtomicU32::new(0);
static PRIO_A_ORDER: AtomicU32 = AtomicU32::new(u32::MAX);
static PRIO_B_ORDER: AtomicU32 = AtomicU32::new(u32::MAX);
static PRIO_C_ORDER: AtomicU32 = AtomicU32::new(u32::MAX);
static PRIO_D_ORDER: AtomicU32 = AtomicU32::new(u32::MAX);
static PRIO_DONE_COUNT: AtomicU32 = AtomicU32::new(0);

extern "C" fn prio_a_op(_ctx: *mut core::ffi::c_void) {
    PRIO_A_ORDER.store(
        PRIO_RUN_ORDER.fetch_add(1, Ordering::SeqCst),
        Ordering::SeqCst,
    );
    PRIO_DONE_COUNT.fetch_add(1, Ordering::SeqCst);
}
extern "C" fn prio_b_op(_ctx: *mut core::ffi::c_void) {
    PRIO_B_ORDER.store(
        PRIO_RUN_ORDER.fetch_add(1, Ordering::SeqCst),
        Ordering::SeqCst,
    );
    PRIO_DONE_COUNT.fetch_add(1, Ordering::SeqCst);
}
extern "C" fn prio_c_op(_ctx: *mut core::ffi::c_void) {
    PRIO_C_ORDER.store(
        PRIO_RUN_ORDER.fetch_add(1, Ordering::SeqCst),
        Ordering::SeqCst,
    );
    PRIO_DONE_COUNT.fetch_add(1, Ordering::SeqCst);
}
extern "C" fn prio_d_op(_ctx: *mut core::ffi::c_void) {
    PRIO_D_ORDER.store(
        PRIO_RUN_ORDER.fetch_add(1, Ordering::SeqCst),
        Ordering::SeqCst,
    );
    PRIO_DONE_COUNT.fetch_add(1, Ordering::SeqCst);
}

// ---------------------------------------------------------------------------
// Cooperative yield-to-priority (rung 5 task 3, retargeted): a NORMAL op that
// models audio_engine::doRecorderCardRoutines' multi-recorder drain — loop
// doing units of "work" (each unit stands in for one recorder's fully-
// processed cardRoutine() dispatch), each followed by a real suspend point
// (`yield_until(None, None)`, mirroring a recorder's SD write, which suspends
// the fiber via `block_on_fiber` while awaiting the transfer) and then a
// `higher_priority_waiting()` check taken only after the unit is fully done;
// the first time that's true, return early, short of `YIELD_TOTAL_UNITS`.
// While it's mid-loop, enqueue a HIGH op and confirm it lands on the ring —
// the next check should trip. Assert: the NORMAL op returned early (didn't
// complete all its units), and the HIGH op ran before a NORMAL
// "continuation" op enqueued right after (mirroring doRecorderCardRoutines
// re-dispatching the drain on its next cadence, resuming with the recorder
// it left off on). This exercises the same ring-scan primitive
// (`higher_priority_waiting`), which is unchanged by the retarget — only the
// C++ caller of the primitive moved. Own statics, independent of every other
// phase.
// ---------------------------------------------------------------------------
const YIELD_TOTAL_UNITS: u32 = 30;
static YIELD_OP_STARTED: AtomicBool = AtomicBool::new(false);
static YIELD_OP_DONE: AtomicBool = AtomicBool::new(false);
static YIELD_UNITS_DONE: AtomicU32 = AtomicU32::new(0);
static YIELD_RETURNED_EARLY: AtomicBool = AtomicBool::new(false);

/// Run-order counter for this phase (mirrors `PRIO_RUN_ORDER` above): each
/// participant stamps the position it actually ran/finished in.
static YIELD_RUN_ORDER: AtomicU32 = AtomicU32::new(0);
static YIELD_HIGH_ORDER: AtomicU32 = AtomicU32::new(u32::MAX);
static YIELD_HIGH_DONE: AtomicBool = AtomicBool::new(false);
static YIELD_CONTINUATION_ORDER: AtomicU32 = AtomicU32::new(u32::MAX);
static YIELD_CONTINUATION_DONE: AtomicBool = AtomicBool::new(false);

extern "C" fn yield_op(_ctx: *mut core::ffi::c_void) {
    YIELD_OP_STARTED.store(true, Ordering::SeqCst);
    for _ in 0..YIELD_TOTAL_UNITS {
        // A unit of "work" with a real suspend point — gives the executor (and
        // therefore submit_pump) a window to land a HIGH enqueue between units,
        // exactly as a real SD write would while this op is genuinely mid-drain.
        crate::fiber::yield_until(None, None);
        YIELD_UNITS_DONE.fetch_add(1, Ordering::SeqCst);
        if crate::fiber::higher_priority_waiting() {
            YIELD_RETURNED_EARLY.store(true, Ordering::SeqCst);
            YIELD_OP_DONE.store(true, Ordering::SeqCst);
            return;
        }
    }
    // Ran to completion without ever observing a HIGH op queued — the race
    // below failed to set up; still mark done so the driving thread's wait
    // doesn't hang (the assertions on YIELD_RETURNED_EARLY will fail instead).
    YIELD_OP_DONE.store(true, Ordering::SeqCst);
}

extern "C" fn yield_high_op(_ctx: *mut core::ffi::c_void) {
    YIELD_HIGH_ORDER.store(
        YIELD_RUN_ORDER.fetch_add(1, Ordering::SeqCst),
        Ordering::SeqCst,
    );
    YIELD_HIGH_DONE.store(true, Ordering::SeqCst);
}

extern "C" fn yield_continuation_op(_ctx: *mut core::ffi::c_void) {
    YIELD_CONTINUATION_ORDER.store(
        YIELD_RUN_ORDER.fetch_add(1, Ordering::SeqCst),
        Ordering::SeqCst,
    );
    YIELD_CONTINUATION_DONE.store(true, Ordering::SeqCst);
}

// ---------------------------------------------------------------------------
// `block_on_fiber` contract (rung 5, the flip): the SD device transfer sites
// (`sd.rs` `deluge_block_read`/`deluge_block_write`) now drive the real SD
// transfer future through `fiber::block_on_fiber` when on the owner fiber —
// suspending the fiber (not parking the whole executor, unlike `block_on`)
// until the future's own Waker (fired by the transfer-completion IRQ, on
// device) or the pump's coarse fallback resumes it. This phase drives that
// SAME `fiber::block_on_fiber` fn against a hand-built Future backed by
// `embassy_sync::waitqueue::AtomicWaker` — the SAME primitive
// `rza1l-hal`'s `sdhi.rs`/`dmac.rs` DMA- and command-completion futures
// actually use (`register(cx.waker())` on `Pending`, `.wake()` from the IRQ
// handler) — rather than `embassy_time::Timer` (tried first; it panics
// under a non-Embassy-task Waker like `block_on_fiber`'s, since it needs
// Embassy's own executor-task waker registration, not just any `Waker` —
// so it would have been the wrong stand-in). A background OS thread stands
// in for the completion IRQ, calling `.wake()` after a short delay. Own
// statics, independent of every other phase.
//
// Asserts: (1) it runs on the fiber throughout; (2) another op accepted onto
// the same ring while it's suspended does NOT start executing until it
// completes — `worker_poll`'s `FIBER_BUSY` gate means only one op ever
// occupies the fiber at a time, so this is the single-owner/no-re-entrancy
// property the whole ladder exists to preserve, now checked against the
// actual Future-driving mechanism rather than the `yield_until` primitive
// the other phases above use; (3) it resolves within a bound tight enough
// to prove the Waker is actually driving it forward, not just an unrelated
// coarse fallback eventually catching up.
// ---------------------------------------------------------------------------
static BOF_STARTED: AtomicBool = AtomicBool::new(false);
static BOF_DONE: AtomicBool = AtomicBool::new(false);
static BOF_IN_OP: AtomicBool = AtomicBool::new(false);
static BOF_ON_OWNER_VIOLATIONS: AtomicU32 = AtomicU32::new(0);
static BOF_SERIALIZATION_VIOLATIONS: AtomicU32 = AtomicU32::new(0);
static BOF_PROBE_RAN_BEFORE_COMPLETE: AtomicBool = AtomicBool::new(false);
static BOF_PROBE_DONE: AtomicBool = AtomicBool::new(false);

/// Waker storage + ready flag for [`BofFuture`], mirroring the shape of
/// `rza1l-hal`'s per-channel DMA/SDHI completion state (an `AtomicWaker`
/// plus a hardware-status bit checked on `poll`).
static BOF_WAKER: AtomicWaker = AtomicWaker::new();
static BOF_READY: AtomicBool = AtomicBool::new(false);

/// A minimal stand-in for the real SD transfer future's shape: `Pending`
/// (registering the waker) until some external event (here, a background
/// thread simulating the completion IRQ) flips [`BOF_READY`] and wakes it.
struct BofFuture;
impl Future for BofFuture {
    type Output = ();
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if BOF_READY.load(Ordering::SeqCst) {
            Poll::Ready(())
        } else {
            BOF_WAKER.register(cx.waker());
            Poll::Pending
        }
    }
}

extern "C" fn block_on_fiber_op(_ctx: *mut core::ffi::c_void) {
    if !crate::sd::deluge_storage_on_owner() {
        BOF_ON_OWNER_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    if BOF_IN_OP.swap(true, Ordering::SeqCst) {
        BOF_SERIALIZATION_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    BOF_STARTED.store(true, Ordering::SeqCst);
    BOF_READY.store(false, Ordering::SeqCst);
    // Stand-in for the completion IRQ: fires the AtomicWaker after a short
    // delay, exactly as the real DMA/SDHI completion handler would.
    std::thread::spawn(|| {
        std::thread::sleep(Duration::from_millis(30));
        BOF_READY.store(true, Ordering::SeqCst);
        BOF_WAKER.wake();
    });
    // The real `fiber::block_on_fiber` fn, driving a real async Future with a
    // genuine Waker-fired completion — the exact mechanism sd.rs's device
    // read/write sites now use for the actual SD transfer future.
    crate::fiber::block_on_fiber(BofFuture);
    if !crate::sd::deluge_storage_on_owner() {
        BOF_ON_OWNER_VIOLATIONS.fetch_add(1, Ordering::SeqCst);
    }
    BOF_IN_OP.store(false, Ordering::SeqCst);
    BOF_DONE.store(true, Ordering::SeqCst);
}

/// Submitted while `block_on_fiber_op` is suspended inside `block_on_fiber`;
/// must NOT run until `block_on_fiber_op` has fully completed (single-owner:
/// only one op occupies the fiber at a time — see `worker_poll`'s
/// `FIBER_BUSY` gate in `fiber.rs`).
extern "C" fn block_on_fiber_probe_op(_ctx: *mut core::ffi::c_void) {
    if !BOF_DONE.load(Ordering::SeqCst) {
        BOF_PROBE_RAN_BEFORE_COMPLETE.store(true, Ordering::SeqCst);
    }
    BOF_PROBE_DONE.store(true, Ordering::SeqCst);
}

// ---------------------------------------------------------------------------
// The submission channel + the two executor-thread pump tasks.
// ---------------------------------------------------------------------------

/// The receiving half, handed to [`submit_pump`] once at startup. `Mutex`
/// just to get it into a `'static` a `#[embassy_executor::task]` (which takes
/// no captures) can reach; only `submit_pump` itself ever locks it after
/// setup, so there is no real contention.
static SUBMIT_RX: Mutex<Option<mpsc::Receiver<Job>>> = Mutex::new(None);

/// Total accepted (non-dropped) enqueues across every phase, incremented by
/// [`submit_pump`] right after a `deluge_worker_run*` call returns `true`. The
/// priority-ordering phase (below) diffs this counter to know its ops have
/// actually landed in `fiber.rs`'s ring (not merely sent down the channel)
/// before it releases the park op that's been holding the fiber.
static SUBMIT_ACCEPTED: AtomicU32 = AtomicU32::new(0);

/// Pulls submissions off the channel and is the ONLY caller of the real
/// `deluge_worker_run` — see the module doc for why that must stay confined
/// to the executor thread. Runs alongside `worker_pump` on the same executor.
#[embassy_executor::task]
async fn submit_pump() {
    loop {
        let job = SUBMIT_RX.lock().unwrap().as_mut().unwrap().try_recv();
        match job {
            Ok((f, ctx, is_sd, is_high)) => {
                // deluge_worker_run* returns bool (dispatch accepted?); the exercise's
                // single-flight submitters never overflow the queue, so ignore it here
                // (beyond the SUBMIT_ACCEPTED tally). is_high routes through the
                // priority entry (checked first — mutually exclusive with is_sd in
                // this exercise), is_sd through the SD-routine entry (which takes the
                // SD_ROUTINE_HELD hold), else the plain one.
                let ctx = ctx as *mut core::ffi::c_void;
                let accepted = if is_high {
                    crate::fiber::deluge_worker_run_priority(f, ctx)
                } else if is_sd {
                    crate::fiber::deluge_worker_run_sd_routine(f, ctx)
                } else {
                    crate::fiber::deluge_worker_run(f, ctx)
                };
                if accepted {
                    SUBMIT_ACCEPTED.fetch_add(1, Ordering::SeqCst);
                }
            }
            Err(TryRecvError::Empty) => embassy_time::Timer::after_millis(1).await,
            Err(TryRecvError::Disconnected) => return,
        }
    }
}

/// The worker pump: drives `fiber::worker_poll()` so a submitted op actually
/// starts/resumes — identical in shape to `scheduler_host_exercise.rs`'s.
#[embassy_executor::task]
async fn worker_pump() {
    use embassy_futures::select::select;
    loop {
        let busy = crate::fiber::worker_poll();
        if busy {
            let _ = select(
                crate::fiber::WORKER_WAKE.wait(),
                embassy_time::Timer::after_millis(2),
            )
            .await;
        } else {
            crate::fiber::WORKER_WAKE.wait().await;
        }
    }
}

/// Poll `cond` until true, sleeping in short increments; panics with `what` if
/// `deadline` passes first — the deadlock watchdog, same as
/// `scheduler_host_exercise.rs`'s.
fn wait_until(deadline: Instant, what: &str, mut cond: impl FnMut() -> bool) {
    loop {
        if cond() {
            return;
        }
        if Instant::now() >= deadline {
            panic!("owner_host: timed out waiting for: {what}");
        }
        std::thread::sleep(Duration::from_millis(2));
    }
}

/// Runs the full exercise: bring up the host executor (`worker_pump` +
/// `submit_pump`), submit a suspending op and confirm the owner-query's
/// outside-an-op half from this (driving) thread while it's parked, then race
/// `NUM_SUBMITTERS` real OS threads submitting `OPS_PER_SUBMITTER` fast ops
/// each through the channel, and assert every contract held throughout.
/// Panics (failing the caller, `#[test]` or plain `fn main()`) on any
/// assertion failure or timeout.
pub fn run() {
    let _ = env_logger::builder().is_test(true).try_init();

    let (tx, rx) = mpsc::channel::<Job>();
    *SUBMIT_RX.lock().unwrap() = Some(rx);

    std::thread::Builder::new()
        .name("deluge-owner-host".into())
        .spawn(|| {
            let executor: &'static mut Executor = Box::leak(Box::new(Executor::new()));
            executor.run(|spawner: Spawner| {
                spawner.spawn(worker_pump().unwrap());
                spawner.spawn(submit_pump().unwrap());
                spawner.spawn(drop_test().unwrap());
            });
        })
        .expect("spawning the host executor thread");

    let deadline = Instant::now() + Duration::from_secs(20);

    // --- bookend: nothing has run yet, definitely not on the owner ---
    assert!(
        !crate::sd::deluge_storage_on_owner(),
        "deluge_storage_on_owner() true before any op ever ran"
    );

    // --- submit the suspending op first, so the 4 submitter threads (below)
    // genuinely race to enqueue their fast ops *while* it occupies the fiber
    // ---
    tx.send((special_op, 0, false, false))
        .expect("send special_op");
    wait_until(deadline, "special_op to start", || {
        SPECIAL_STARTED.load(Ordering::SeqCst)
    });
    // Give worker_poll time to actually park it (return from resume() after
    // yield_until's switch) rather than racing our own check against it.
    std::thread::sleep(Duration::from_millis(50));

    // --- the genuinely new assertion: the owner-query is false from a
    // non-executor (this) thread while an op is in flight but not literally
    // on the fiber (suspended, mid-yield) ---
    assert!(
        !crate::sd::deluge_storage_on_owner(),
        "deluge_storage_on_owner() true on the driving thread while special_op \
         was suspended off the fiber — the query should only ever be true on \
         the executor thread while genuinely executing an op body"
    );

    // --- several submitter threads racing to enqueue fast ops from
    // non-executor threads, while special_op is still parked ---
    let submitters: Vec<_> = (0..NUM_SUBMITTERS)
        .map(|thread_id| {
            let tx = tx.clone();
            std::thread::Builder::new()
                .name(format!("owner-submitter-{thread_id}"))
                .spawn(move || {
                    for i in 0..OPS_PER_SUBMITTER {
                        tx.send((fast_op, thread_id, false, false))
                            .expect("send fast_op");
                        // Throttle to at most one outstanding op per thread —
                        // see the module doc: this keeps at most
                        // NUM_SUBMITTERS (== QUEUE_CAP) jobs live at once, so
                        // deluge_worker_run's fixed-capacity ring never has
                        // to silently drop a submission.
                        let expected = i + 1;
                        let deadline = Instant::now() + Duration::from_secs(20);
                        wait_until(
                            deadline,
                            "this submitter's fast_op to complete before sending the next",
                            || {
                                PER_SUBMITTER_COMPLETED[thread_id].load(Ordering::SeqCst)
                                    >= expected
                            },
                        );
                    }
                })
                .expect("spawning a submitter thread")
        })
        .collect();

    // --- let special_op resume and finish ---
    SPECIAL_GATE.store(true, Ordering::SeqCst);
    wait_until(deadline, "special_op to finish", || {
        SPECIAL_DONE.load(Ordering::SeqCst)
    });

    for h in submitters {
        h.join().expect("submitter thread panicked");
    }

    // --- all ops (fast + special) completed within the wall-clock deadline ---
    wait_until(deadline, "all ops to complete", || {
        COMPLETED.load(Ordering::SeqCst) >= TOTAL_OPS
    });

    // --- serialization + owner-query contracts held throughout ---
    assert_eq!(
        SERIALIZATION_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "the Owner ran two ops concurrently (serialization contract broken)"
    );
    assert_eq!(
        ON_OWNER_INSIDE_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "deluge_storage_on_owner() was false while an op body was genuinely \
         executing on the fiber (owner-query inside-an-op contract broken)"
    );

    // --- bookend: everything's done, back to not-on-the-owner ---
    assert!(
        !crate::sd::deluge_storage_on_owner(),
        "deluge_storage_on_owner() true after every op completed"
    );

    // --- SD-routine hold: engaged at enqueue, released at completion ---
    // No SD-routine op has run, so no hold is held.
    assert!(
        !crate::fiber::sd_routine_held(),
        "sd_routine_held() true before any SD-routine op ran"
    );
    let sd_deadline = Instant::now() + Duration::from_secs(20);
    // Submit the suspending SD-routine op through the executor-thread submitter.
    tx.send((sd_routine_op, 0, true, false))
        .expect("send sd_routine_op");
    wait_until(sd_deadline, "sd_routine_op to start", || {
        SD_OP_STARTED.load(Ordering::SeqCst)
    });
    // While it is in flight (suspended on its gate), the hold must be visible.
    wait_until(
        sd_deadline,
        "sd_routine_held() true while op in flight",
        || crate::fiber::sd_routine_held(),
    );
    // Release it; once it completes the hold must clear.
    SD_OP_GATE.store(true, Ordering::SeqCst);
    wait_until(sd_deadline, "sd_routine_op to finish", || {
        SD_OP_DONE.load(Ordering::SeqCst)
    });
    wait_until(
        sd_deadline,
        "sd_routine_held() false after completion",
        || !crate::fiber::sd_routine_held(),
    );

    // --- drop-retry contract: a full ring refuses, a drained ring accepts again (rung 4a) ---
    let drop_deadline = Instant::now() + Duration::from_secs(20);
    DROP_TEST_GO.store(true, Ordering::SeqCst);
    wait_until(drop_deadline, "drop_test to finish", || {
        DROP_TEST_DONE.load(Ordering::SeqCst)
    });
    assert_eq!(
        DROP_ACCEPTED.load(Ordering::SeqCst),
        QUEUE_CAP_MIRROR as u32,
        "a burst of CAP+1 synchronous enqueues should fill the ring to exactly CAP"
    );
    assert_eq!(
        DROP_DROPPED.load(Ordering::SeqCst),
        1,
        "the enqueue past a full ring should be refused (false) exactly once"
    );
    assert!(
        DROP_RETRY_OK.load(Ordering::SeqCst),
        "after the ring drained, a resubmission should be accepted again (drop-retry)"
    );

    // --- render-then-complete (rung 4b, shape 1): submit render_op, let it
    // park (modelling "in flight, rendering"), then resume it and assert the
    // UI-state write landed exactly once, on the fiber, serialized ---
    assert_eq!(
        UI_STATE_READY.load(Ordering::SeqCst),
        0,
        "UI state written before render_op ever ran"
    );
    let render_deadline = Instant::now() + Duration::from_secs(20);
    tx.send((render_op, 0, false, false))
        .expect("send render_op");
    wait_until(render_deadline, "render_op to start", || {
        RENDER_STARTED.load(Ordering::SeqCst)
    });
    // While parked, the write must not have landed yet — it only happens on
    // resume/completion, not on entry.
    assert_eq!(
        UI_STATE_READY.load(Ordering::SeqCst),
        0,
        "UI state written before render_op resumed and completed"
    );
    RENDER_GATE.store(true, Ordering::SeqCst);
    wait_until(render_deadline, "render_op to finish", || {
        RENDER_DONE.load(Ordering::SeqCst)
    });
    assert_eq!(
        UI_STATE_READY.load(Ordering::SeqCst),
        1,
        "UI state should read 'ready' after render_op completed"
    );
    assert_eq!(
        UI_STATE_WRITES.load(Ordering::SeqCst),
        1,
        "the UI-state write should happen exactly once"
    );
    assert_eq!(
        RENDER_ON_OWNER_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "deluge_storage_on_owner() was false while render_op was genuinely \
         executing on the fiber"
    );
    assert_eq!(
        RENDER_SERIALIZATION_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "render_op overlapped with another op body (serialization contract broken)"
    );

    // --- optimistic-open back-out (rung 4b, shape 2): submit backout_op,
    // which commits optimistically then unwinds a simulated listing failure
    // in the same op body — assert the unwind ran on the fiber and the
    // "committed" flag is not left set afterward ---
    let backout_deadline = Instant::now() + Duration::from_secs(20);
    tx.send((backout_op, 0, false, false))
        .expect("send backout_op");
    wait_until(backout_deadline, "backout_op to finish", || {
        BACKOUT_DONE.load(Ordering::SeqCst)
    });
    assert!(
        OPEN_FAILED.load(Ordering::SeqCst),
        "backout_op should have taken the simulated failure path"
    );
    assert!(
        UNWIND_RAN.load(Ordering::SeqCst),
        "the unwind (onListingFailed) should have run"
    );
    assert_eq!(
        BACKOUT_ON_OWNER_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "deluge_storage_on_owner() was false while backout_op was genuinely \
         executing on the fiber"
    );
    assert_eq!(
        UNWIND_ON_OWNER_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "the unwind (onListingFailed) did not run on the fiber"
    );
    assert_eq!(
        BACKOUT_SERIALIZATION_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "backout_op overlapped with another op body (serialization contract broken)"
    );
    assert!(
        !OPEN_COMMITTED.load(Ordering::SeqCst),
        "browser-open-committed flag left set after a failed+unwound optimistic open"
    );

    // --- two-level priority (rung 5 task 2): park a NORMAL op so the fiber is
    // busy, enqueue NORMAL-A, HIGH-B, HIGH-C, NORMAL-D (in that order) while
    // it's parked, then release and assert HIGH ran before NORMAL with FIFO
    // held within each level ---
    let prio_deadline = Instant::now() + Duration::from_secs(20);
    tx.send((prio_park_op, 0, false, false))
        .expect("send prio_park_op");
    wait_until(prio_deadline, "prio_park_op to start", || {
        PRIO_PARK_STARTED.load(Ordering::SeqCst)
    });
    // Give worker_poll time to actually park it (return from resume() after
    // yield_until's switch) rather than racing our own check against it —
    // mirrors the special_op bookend above.
    std::thread::sleep(Duration::from_millis(50));

    let accepted_before = SUBMIT_ACCEPTED.load(Ordering::SeqCst);
    tx.send((prio_a_op, 0, false, false))
        .expect("send prio-A (normal)");
    tx.send((prio_b_op, 0, false, true))
        .expect("send prio-B (high)");
    tx.send((prio_c_op, 0, false, true))
        .expect("send prio-C (high)");
    tx.send((prio_d_op, 0, false, false))
        .expect("send prio-D (normal)");
    // Confirm all four are actually enqueued onto fiber.rs's ring (not merely
    // sent down the channel) before releasing the park — otherwise the gate
    // could open before D lands, and the enqueue order this test relies on
    // (A, B, C, D) wouldn't be settled yet.
    wait_until(
        prio_deadline,
        "prio-A/B/C/D to all be accepted onto the ring",
        || SUBMIT_ACCEPTED.load(Ordering::SeqCst) >= accepted_before + 4,
    );

    PRIO_PARK_GATE.store(true, Ordering::SeqCst);
    wait_until(prio_deadline, "prio_park_op to finish", || {
        PRIO_PARK_DONE.load(Ordering::SeqCst)
    });
    wait_until(prio_deadline, "prio-A/B/C/D to all complete", || {
        PRIO_DONE_COUNT.load(Ordering::SeqCst) >= 4
    });

    let a = PRIO_A_ORDER.load(Ordering::SeqCst);
    let b = PRIO_B_ORDER.load(Ordering::SeqCst);
    let c = PRIO_C_ORDER.load(Ordering::SeqCst);
    let d = PRIO_D_ORDER.load(Ordering::SeqCst);
    assert!(
        b < a && c < a,
        "HIGH ops (B, C) should both run before NORMAL op A despite A \
         enqueuing first (a={a}, b={b}, c={c})"
    );
    assert!(
        b < d && c < d,
        "HIGH ops (B, C) should both run before NORMAL op D (b={b}, c={c}, d={d})"
    );
    assert!(
        b < c,
        "FIFO within the HIGH level: B enqueued before C, so B must run \
         first (b={b}, c={c})"
    );
    assert!(
        a < d,
        "FIFO within the NORMAL level: A enqueued before D, so A must run \
         first (a={a}, d={d})"
    );

    // --- cooperative yield-to-priority (rung 5 task 3): submit yield_op, wait
    // for it to start, then queue a HIGH op and confirm it lands on the ring
    // while yield_op is still mid-loop. Note: unlike deluge_storage_on_owner
    // (a single AtomicBool, safe from any thread — see the module doc),
    // higher_priority_waiting() reads the same unsynchronized `QUEUE` ring
    // enqueue/dequeue do, so it's only called here from op bodies running on
    // the fiber (the executor thread), never from this (driving) thread. ---
    let yield_deadline = Instant::now() + Duration::from_secs(20);
    tx.send((yield_op, 0, false, false)).expect("send yield_op");
    wait_until(yield_deadline, "yield_op to start", || {
        YIELD_OP_STARTED.load(Ordering::SeqCst)
    });

    let accepted_before = SUBMIT_ACCEPTED.load(Ordering::SeqCst);
    tx.send((yield_high_op, 0, false, true))
        .expect("send yield_high_op (high)");
    wait_until(
        yield_deadline,
        "yield_high_op to be accepted onto the ring",
        || SUBMIT_ACCEPTED.load(Ordering::SeqCst) >= accepted_before + 1,
    );

    // --- yield_op must observe the queued HIGH op and return early ---
    wait_until(yield_deadline, "yield_op to return (early)", || {
        YIELD_OP_DONE.load(Ordering::SeqCst)
    });
    assert!(
        YIELD_RETURNED_EARLY.load(Ordering::SeqCst),
        "yield_op should have observed higher_priority_waiting() == true and \
         returned early rather than running to completion"
    );
    let units_done = YIELD_UNITS_DONE.load(Ordering::SeqCst);
    assert!(
        units_done < YIELD_TOTAL_UNITS,
        "yield_op should NOT have completed all {YIELD_TOTAL_UNITS} units — \
         higher_priority_waiting() should have short-circuited it early; \
         completed {units_done}"
    );

    // --- re-dispatch a "continuation" NORMAL op, mirroring
    // doRecorderCardRoutines resuming the drain on its next cadence after an
    // early return — the already-queued HIGH op must run ahead of it ---
    tx.send((yield_continuation_op, 0, false, false))
        .expect("send yield_continuation_op");
    wait_until(yield_deadline, "yield_high_op to finish", || {
        YIELD_HIGH_DONE.load(Ordering::SeqCst)
    });
    wait_until(yield_deadline, "yield_continuation_op to finish", || {
        YIELD_CONTINUATION_DONE.load(Ordering::SeqCst)
    });
    let high_order = YIELD_HIGH_ORDER.load(Ordering::SeqCst);
    let cont_order = YIELD_CONTINUATION_ORDER.load(Ordering::SeqCst);
    assert!(
        high_order < cont_order,
        "the HIGH op should have run before the re-dispatched NORMAL \
         continuation (high_order={high_order}, cont_order={cont_order})"
    );

    // --- block_on_fiber (rung 5, the flip): submit block_on_fiber_op (which
    // drives a hand-built, AtomicWaker-backed BofFuture through the actual
    // fiber::block_on_fiber fn — the same fn sd.rs's device read/write sites
    // now use for the real SD transfer future; see the header above for why
    // embassy_time::Timer was tried and rejected), then while it's suspended
    // submit a probe op and confirm it doesn't start until block_on_fiber_op
    // completes, and that completion happens promptly (Waker-driven, not
    // just an unrelated coarse fallback) ---
    let bof_deadline = Instant::now() + Duration::from_secs(20);
    let bof_start = Instant::now();
    tx.send((block_on_fiber_op, 0, false, false))
        .expect("send block_on_fiber_op");
    wait_until(bof_deadline, "block_on_fiber_op to start", || {
        BOF_STARTED.load(Ordering::SeqCst)
    });
    tx.send((block_on_fiber_probe_op, 0, false, false))
        .expect("send block_on_fiber_probe_op");
    wait_until(bof_deadline, "block_on_fiber_op to finish", || {
        BOF_DONE.load(Ordering::SeqCst)
    });
    let bof_elapsed = bof_start.elapsed();
    wait_until(bof_deadline, "block_on_fiber_probe_op to finish", || {
        BOF_PROBE_DONE.load(Ordering::SeqCst)
    });
    assert_eq!(
        BOF_ON_OWNER_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "deluge_storage_on_owner() was false while block_on_fiber_op was \
         genuinely executing on the fiber"
    );
    assert_eq!(
        BOF_SERIALIZATION_VIOLATIONS.load(Ordering::SeqCst),
        0,
        "block_on_fiber_op overlapped with another op body (serialization \
         contract broken)"
    );
    assert!(
        !BOF_PROBE_RAN_BEFORE_COMPLETE.load(Ordering::SeqCst),
        "block_on_fiber_probe_op ran before block_on_fiber_op completed — a \
         second op started while the first was still suspended inside \
         block_on_fiber (single-owner/no-re-entrancy contract broken)"
    );
    assert!(
        bof_elapsed < Duration::from_millis(500),
        "block_on_fiber_op took {bof_elapsed:?} to resolve a 30ms Timer — \
         suggests it isn't being driven by its Waker/the pump promptly"
    );
}
