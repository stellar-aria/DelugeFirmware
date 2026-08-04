//! A discrete-event driver spike: can a `MockDriver`-style discrete-event
//! loop — run the executor to quiescence, jump virtual time to the next due
//! timer deadline, repeat — deterministically drive Embassy tasks with
//! near-zero wall-clock cost?
//!
//! # Why this doesn't use `embassy_executor::Executor` (`platform-std`)
//!
//! `deluge-bsp-rust`'s host build enables embassy-executor's `platform-std` +
//! `executor-thread` features. Those compile a fixed, crate-internal
//! `#[unsafe(export_name = "__pender")]` function (see
//! `embassy-executor/src/platform/std.rs`) that assumes its `context: *mut
//! ()` argument is a `&'static Signaler` (a private `Mutex<bool>` +
//! `Condvar`) and unconditionally transmutes it as such — there is exactly
//! one `__pender` per binary (Rust `extern "Rust"` symbols are resolved by
//! name at link time, the same mechanism `embassy-time`'s own driver uses —
//! see the `PeekableMockDriver` doc below), so nothing else in the process
//! can intercept or replace it. Its `Executor::run_until(init, done)` loop
//! (`poll(); if done() break; else signaler.wait()`) genuinely converges to
//! quiescence with ~zero *added* wall time (signal-before-wait means a
//! pending wake makes `wait()` return immediately, not block), but it gives
//! an external driver no way to observe "the executor just went idle" so it
//! knows it's safe to call `MockDriver::advance()` — that fact only exists
//! transiently inside the condvar, never surfaced.
//!
//! `embassy-executor`'s `raw` module is the documented escape hatch for
//! exactly this ("if you need a different executor, you must not enable
//! `arch-xx` features" — `raw/mod.rs`). This binary enables NO `platform-*`
//! feature at all (see `Cargo.toml`), so the crate defines no `__pender`,
//! and this file supplies one that just sets an `AtomicBool` — no thread,
//! no parking, nothing to observe indirectly. The driver loop below polls,
//! checks the flag itself, and only calls `MockDriver`-equivalent `advance`
//! once a poll pass truly changed nothing. This is NOT a `platform-std`
//! variant; it is `platform-std`'s replacement for discrete-event use. The
//! verdict this spike reaches for Lens 1 is about THIS shape, not about
//! coaxing `platform-std` itself into stepping.

#![feature(impl_trait_in_assoc_type)]

use core::cell::RefCell;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use core::task::Waker;
use std::time::Instant as WallInstant;

use critical_section::Mutex as CsMutex;
use embassy_executor::raw;
use embassy_time::{Duration, Timer};
use embassy_time_driver::Driver as _;
use embassy_time_queue_utils::Queue;

/// A peekable sibling of `embassy_time::MockDriver` (the `mock-driver`
/// feature's shipped type). Built on the exact same public SPI MockDriver
/// itself uses (`embassy_time_driver::Driver` + `embassy_time_queue_utils::Queue`,
/// registered via the documented `time_driver_impl!` macro — see
/// `embassy-time-driver/src/lib.rs`'s "Implementing a driver" section) — this
/// is not a fork or a patch, just an independent implementation of the same
/// trait.
///
/// The reason it exists instead of using `embassy_time::MockDriver` directly:
/// `MockDriver` exposes `advance(Duration)` and `now()`/`get()` but has NO
/// "what's the next scheduled deadline" accessor (checked: its single field
/// is a private `critical_section::Mutex<RefCell<InnerMockDriver>>` — see
/// `embassy-time-0.5.1/src/driver_mock.rs` — with no peek method, and
/// `embassy_time_queue_utils::Queue::next_expiration(now)` — the only
/// function that could answer "what's next" — both PEEKS and FIRES in one
/// call, waking every item with `expires_at <= now`. Upstream MockDriver
/// calls it internally but never surfaces the return value.). A real
/// jump-to-exact-deadline discrete-event loop needs that peek; this struct
/// adds it in ~15 extra lines over what MockDriver already has internally.
///
/// The peek is safe/idempotent specifically because the driver loop only
/// ever calls it right after a quiescence point: every prior `advance_ticks`
/// call already fired everything due at-or-before the new `now`, so calling
/// `next_expiration(now)` again finds nothing `<= now` to fire — it just
/// walks the queue, returns the soonest `expires_at`, and mutates nothing
/// observable.
struct PeekableMockDriver(CsMutex<RefCell<Inner>>);

struct Inner {
    now: u64,
    queue: Queue,
}

embassy_time_driver::time_driver_impl!(static DRIVER: PeekableMockDriver = PeekableMockDriver(
    CsMutex::new(RefCell::new(Inner { now: 0, queue: Queue::new() }))
));

impl embassy_time_driver::Driver for PeekableMockDriver {
    fn now(&self) -> u64 {
        critical_section::with(|cs| self.0.borrow_ref(cs).now)
    }

    fn schedule_wake(&self, at: u64, waker: &Waker) {
        critical_section::with(|cs| {
            let mut inner = self.0.borrow_ref_mut(cs);
            inner.queue.schedule_wake(at, waker);
            let now = inner.now;
            inner.queue.next_expiration(now);
        });
    }
}

impl PeekableMockDriver {
    fn get() -> &'static Self {
        &DRIVER
    }

    /// Jump virtual time forward by `ticks`, synchronously firing (waking)
    /// every timer whose deadline is now `<= now`. May wake more than one
    /// task in one call if deadlines coincide.
    fn advance_ticks(&self, ticks: u64) {
        critical_section::with(|cs| {
            let mut inner = self.0.borrow_ref_mut(cs);
            inner.now += ticks;
            let now = inner.now;
            inner.queue.next_expiration(now);
        });
    }

    /// The next scheduled wake tick, or `None` if no task has an outstanding
    /// timer at all (only safe to trust at a quiescence point — see the
    /// struct doc).
    fn peek_next_deadline(&self) -> Option<u64> {
        critical_section::with(|cs| {
            let mut inner = self.0.borrow_ref_mut(cs);
            let now = inner.now;
            let next = inner.queue.next_expiration(now);
            (next != u64::MAX).then_some(next)
        })
    }
}

// ---------------------------------------------------------------------------
// Three periodic tasks (7ms/10ms/30ms, deliberately not all divisors of one
// another — 10 & 30 coincide periodically, 7 doesn't line up with either) —
// standing in for Lens 1's SD-completion / audio-drain / margin-timer style
// waits. Each just loops `Timer::after(..).await` + increments a counter.
// ---------------------------------------------------------------------------

static COUNTER_7MS: AtomicU32 = AtomicU32::new(0);
static COUNTER_10MS: AtomicU32 = AtomicU32::new(0);
static COUNTER_30MS: AtomicU32 = AtomicU32::new(0);

#[embassy_executor::task]
async fn task_7ms() {
    loop {
        Timer::after(Duration::from_millis(7)).await;
        COUNTER_7MS.fetch_add(1, Ordering::SeqCst);
    }
}

#[embassy_executor::task]
async fn task_10ms() {
    loop {
        Timer::after(Duration::from_millis(10)).await;
        COUNTER_10MS.fetch_add(1, Ordering::SeqCst);
    }
}

#[embassy_executor::task]
async fn task_30ms() {
    loop {
        Timer::after(Duration::from_millis(30)).await;
        COUNTER_30MS.fetch_add(1, Ordering::SeqCst);
    }
}

/// Our own pender (see the module doc for why we're allowed/required to
/// define this): no thread, no parking — just a flag the driver loop polls
/// itself, synchronously, in between calls to `raw::Executor::poll()`.
static PENDED: AtomicBool = AtomicBool::new(false);

#[unsafe(export_name = "__pender")]
fn pender(_context: *mut ()) {
    PENDED.store(true, Ordering::SeqCst);
}

/// Virtual duration to simulate.
const TARGET_MS: u64 = 500;

fn main() {
    // SAFETY: `raw::Executor` requires `&'static`; leaking a Box is the
    // standard way to get that from a `fn main` that doesn't itself run
    // forever (see `embassy_executor::Executor::run`'s own doc for the same
    // pattern, just without the StaticCell/`static mut` device-side options).
    let executor: &'static raw::Executor =
        Box::leak(Box::new(raw::Executor::new(std::ptr::null_mut())));
    let spawner = executor.spawner();
    spawner.spawn(task_7ms().unwrap());
    spawner.spawn(task_10ms().unwrap());
    spawner.spawn(task_30ms().unwrap());

    let driver = PeekableMockDriver::get();
    let target_ticks = Duration::from_millis(TARGET_MS).as_ticks();
    let wall_start = WallInstant::now();

    let mut quiescence_passes = 0u64;
    let mut advances = 0u64;
    loop {
        // (a) Run the executor to quiescence: keep polling until a pass
        // enqueues nothing new. `PENDED` is set by our `__pender` whenever
        // `raw::Executor`'s internal run-queue `enqueue()` fires (a task got
        // (re)woken) — see `embassy-executor/src/raw/mod.rs`'s
        // `SyncExecutor::enqueue`.
        loop {
            PENDED.store(false, Ordering::SeqCst);
            // SAFETY: single-threaded, not called reentrantly (no nested
            // `poll()` call happens from within a task body here).
            unsafe { executor.poll() };
            quiescence_passes += 1;
            if !PENDED.load(Ordering::SeqCst) {
                break;
            }
        }

        // (b) Read the next due deadline.
        let Some(next) = driver.peek_next_deadline() else {
            println!("[spike] no task has an outstanding timer — stopping");
            break;
        };
        if next > target_ticks {
            println!(
                "[spike] next deadline ({next} ticks) is past the {TARGET_MS}ms virtual target — stopping"
            );
            break;
        }

        // (c) Advance the virtual clock to exactly that deadline (never
        // overshoots, never skips a coincident deadline).
        let now = driver.now();
        driver.advance_ticks(next - now);
        advances += 1;
        // (d) repeat.
    }

    let wall_elapsed = wall_start.elapsed();
    let c7 = COUNTER_7MS.load(Ordering::SeqCst);
    let c10 = COUNTER_10MS.load(Ordering::SeqCst);
    let c30 = COUNTER_30MS.load(Ordering::SeqCst);

    println!("[spike] virtual time simulated: {TARGET_MS}ms");
    println!("[spike] quiescence poll() passes: {quiescence_passes}, clock advances: {advances}");
    println!("[spike] counters: 7ms={c7} 10ms={c10} 30ms={c30}");
    println!("[spike] wall-clock elapsed: {wall_elapsed:?}");

    // Deterministic virtual-time order: exact floor(TARGET_MS / period)
    // fires (fence-post: a deadline landing exactly on TARGET_MS still
    // fires, since the loop's stop check is `next > target_ticks`, not
    // `>=`).
    let expect = |period_ms: u64| TARGET_MS / period_ms;
    assert_eq!(
        c7,
        expect(7) as u32,
        "7ms task fired the wrong number of times"
    );
    assert_eq!(
        c10,
        expect(10) as u32,
        "10ms task fired the wrong number of times"
    );
    assert_eq!(
        c30,
        expect(30) as u32,
        "30ms task fired the wrong number of times"
    );

    // Virtual, not real: simulating 500ms of scheduled activity must not
    // cost anywhere near 500ms — or even 1ms — of wall time.
    assert!(
        wall_elapsed.as_millis() < 50,
        "wall-clock elapsed ({wall_elapsed:?}) is suspiciously large for a virtual-time loop \
         — this would indicate real sleeping/parking happened somewhere"
    );

    println!(
        "[spike] PASS: {TARGET_MS}ms of virtual time simulated deterministically in {wall_elapsed:?} of wall time"
    );
}
