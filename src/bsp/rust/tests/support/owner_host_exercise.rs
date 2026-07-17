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

use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::mpsc::{self, TryRecvError};
use std::time::{Duration, Instant};

use embassy_executor::{Executor, Spawner};

/// A queued submission, handed from a submitter OS thread to [`submit_pump`]
/// over the channel: the op function plus a `usize`-encoded context (avoids a
/// raw-pointer field, which would make the tuple `!Send`).
type Job = (extern "C" fn(*mut core::ffi::c_void), usize);

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

// ---------------------------------------------------------------------------
// The submission channel + the two executor-thread pump tasks.
// ---------------------------------------------------------------------------

/// The receiving half, handed to [`submit_pump`] once at startup. `Mutex`
/// just to get it into a `'static` a `#[embassy_executor::task]` (which takes
/// no captures) can reach; only `submit_pump` itself ever locks it after
/// setup, so there is no real contention.
static SUBMIT_RX: Mutex<Option<mpsc::Receiver<Job>>> = Mutex::new(None);

/// Pulls submissions off the channel and is the ONLY caller of the real
/// `deluge_worker_run` — see the module doc for why that must stay confined
/// to the executor thread. Runs alongside `worker_pump` on the same executor.
#[embassy_executor::task]
async fn submit_pump() {
    loop {
        let job = SUBMIT_RX.lock().unwrap().as_mut().unwrap().try_recv();
        match job {
            Ok((f, ctx)) => crate::fiber::deluge_worker_run(f, ctx as *mut core::ffi::c_void),
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
    tx.send((special_op, 0)).expect("send special_op");
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
                        tx.send((fast_op, thread_id)).expect("send fast_op");
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
}
