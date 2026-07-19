//! Exercise for the `sim_latency` feature (streaming-underrun harness):
//! proves that a host SD read issued ON THE WORKER FIBER genuinely SUSPENDS
//! the fiber — the executor keeps running other tasks while the read is
//! modeled-in-flight — for roughly the modeled per-transfer delay, then
//! resumes with the correct file-image data. With `sim_latency` off, `sd.rs`'s
//! host path is `block_on`-only and never suspends; this exercise is the "it
//! now suspends" proof.
//!
//! Same `#[path]`-included shape as `owner_host_exercise.rs`'s: driven by the
//! real `fiber.rs` (`deluge_worker_run`/`worker_poll`) and `sd.rs`
//! (`deluge_block_read`/`deluge_block_write`, plus the `sim_latency` module
//! under test) via `crate::fiber`/`crate::sd`, declared by whichever crate
//! root includes this file with `#[path]`. Only one entry point is needed
//! here (a plain `cargo test`; no TSan variant) — see `tests/sim_latency_host.rs`.
//!
//! Submission discipline mirrors `owner_host_exercise.rs`'s: `deluge_worker_run`'s
//! enqueue ring is documented as callable only from the executor thread (see
//! that file's module doc), so the driving (test) thread hands the op to
//! [`submit_pump`] over a channel rather than calling it directly.
//!
//! ## HIGH-priority dispatch: the `request_pump` shape
//!
//! The first block of [`run`] (above) proves suspension via
//! `deluge_worker_run` (NORMAL). The streaming loader's real host dispatch path
//! (`deluge::audio::stream::loader::request_pump`, `loader.cpp:158`) is
//! different in one respect: once off the storage owner
//! (`deluge_storage_on_owner()` false — always true for a task-runner
//! calling `request_pump` on host, since nothing but `worker_poll` ever
//! starts the fiber), it dispatches via `g_loader_coalescer.request(...)` — a
//! `deluge::storage::Coalescer{sd_routine: false, priority: true}` — which
//! forwards to `Owner::run_priority`, which is a one-line passthrough to
//! `deluge_worker_run_priority` (HIGH), not `deluge_worker_run` (NORMAL):
//!
//! ```cpp
//! // storage/owner.cpp
//! bool Owner::run_priority(void (*fn)(void*), void* ctx) {
//!     return deluge_worker_run_priority(fn, ctx);
//! }
//! // Coalescer::request, priority_ == true:
//! dispatched = Owner::run_priority(&Coalescer::run_and_release, this);
//! ```
//!
//! Both `Owner::run_priority` and `Coalescer::request`'s `priority_` branch
//! are trivial passthroughs with no logic of their own beyond the
//! single-flight in-flight guard (irrelevant to suspension timing) — so
//! calling [`crate::fiber::deluge_worker_run_priority`] directly, as [`run`]'s
//! HIGH-priority block does (via [`submit_pump_priority`]/[`priority_read_op`]),
//! exercises the exact same queue path `request_pump` reaches, without
//! needing a real queued `StreamedChunk` (which would need the full C++
//! `deluge_app` linked, out of scope here) or duplicating
//! `owner_host_exercise.rs`'s existing HIGH-vs-NORMAL ordering coverage
//! (`deluge_worker_run_priority` dequeuing ahead of NORMAL ops is already
//! proven there — this block's only new claim is "HIGH dispatch +
//! `sim_latency` suspension compose correctly").
#![cfg(all(not(target_os = "none"), feature = "sim_latency"))]

use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::mpsc::{self, TryRecvError};
use std::time::{Duration, Instant};

use embassy_executor::{Executor, Spawner};
use embassy_time::Timer;

/// Sector this exercise reads/writes. Arbitrary but distinct from other host
/// sd exercises/tests in case a shared backing image is ever reused (this
/// exercise also isolates its own backing file — see `run`).
const TEST_SECTOR: u32 = 4000;
const PATTERN_LEN: usize = 512;

/// Modeled-latency parameters for this exercise: throughput is set high
/// enough that the `bytes / throughput` term is negligible, so the expected
/// delay is dominated by, and close to, the fixed command overhead —
/// deterministic and generous enough to be robust against host scheduling
/// jitter under a busy CI machine.
const OVERHEAD_US: u32 = 150_000; // 150ms
const THROUGHPUT_BYTES_PER_SEC: u32 = 1_000_000_000;

/// Ticks every [`COUNTER_PERIOD_MS`] on the SAME executor as the fiber pump —
/// the "independent task" whose progress proves the executor kept running
/// other work while the fiber was suspended inside the modeled read, rather
/// than the process being synchronously blocked.
static COUNTER: AtomicU32 = AtomicU32::new(0);
const COUNTER_PERIOD_MS: u64 = 5;

#[embassy_executor::task]
async fn counter_pump() {
    loop {
        Timer::after_millis(COUNTER_PERIOD_MS).await;
        COUNTER.fetch_add(1, Ordering::SeqCst);
    }
}

/// Drives `fiber::worker_poll()` — same shape as `owner_host_exercise.rs`'s
/// `worker_pump`.
#[embassy_executor::task]
async fn worker_pump() {
    use embassy_futures::select::select;
    loop {
        let busy = crate::fiber::worker_poll();
        if busy {
            let _ = select(crate::fiber::WORKER_WAKE.wait(), Timer::after_millis(2)).await;
        } else {
            crate::fiber::WORKER_WAKE.wait().await;
        }
    }
}

type Job = extern "C" fn(*mut core::ffi::c_void);

/// The receiving half, handed to [`submit_pump`] once at startup — same
/// pattern as `owner_host_exercise.rs`'s `SUBMIT_RX`.
static SUBMIT_RX: Mutex<Option<mpsc::Receiver<Job>>> = Mutex::new(None);

/// The ONLY caller of `deluge_worker_run` (see the module doc above for why
/// that must stay confined to the executor thread).
#[embassy_executor::task]
async fn submit_pump() {
    loop {
        let job = SUBMIT_RX.lock().unwrap().as_mut().unwrap().try_recv();
        match job {
            Ok(f) => {
                crate::fiber::deluge_worker_run(f, core::ptr::null_mut());
            }
            Err(TryRecvError::Empty) => Timer::after_millis(1).await,
            Err(TryRecvError::Disconnected) => return,
        }
    }
}

/// HIGH-priority-dispatch sibling of [`SUBMIT_RX`]/[`submit_pump`]: the ONLY
/// caller of `deluge_worker_run_priority` — the HIGH-priority entry point
/// `request_pump`'s off-owner dispatch (via `Owner::run_priority`/
/// `Coalescer`) actually reaches on host. See the module doc's
/// "HIGH-priority dispatch" section for why exercising this call directly is
/// a faithful proxy for `request_pump`'s real dispatch shape.
static SUBMIT_RX_PRIORITY: Mutex<Option<mpsc::Receiver<Job>>> = Mutex::new(None);

#[embassy_executor::task]
async fn submit_pump_priority() {
    loop {
        let job = SUBMIT_RX_PRIORITY
            .lock()
            .unwrap()
            .as_mut()
            .unwrap()
            .try_recv();
        match job {
            Ok(f) => {
                crate::fiber::deluge_worker_run_priority(f, core::ptr::null_mut());
            }
            Err(TryRecvError::Empty) => Timer::after_millis(1).await,
            Err(TryRecvError::Disconnected) => return,
        }
    }
}

static READ_STARTED: AtomicBool = AtomicBool::new(false);
static READ_DONE: AtomicBool = AtomicBool::new(false);
static READ_STATUS_OK: AtomicBool = AtomicBool::new(false);
static READ_DATA_OK: AtomicBool = AtomicBool::new(false);
static READ_WAS_ON_FIBER: AtomicBool = AtomicBool::new(false);

fn pattern() -> [u8; PATTERN_LEN] {
    let mut p = [0u8; PATTERN_LEN];
    for (i, b) in p.iter_mut().enumerate() {
        *b = (i as u8).wrapping_mul(97).wrapping_add(13);
    }
    p
}

/// The op body: runs on the fiber (via `worker_poll`/`start`), issues the
/// REAL `deluge_block_read` C-ABI call, and records what it observed.
extern "C" fn read_op(_ctx: *mut core::ffi::c_void) {
    READ_WAS_ON_FIBER.store(crate::sd::deluge_storage_on_owner(), Ordering::SeqCst);
    READ_STARTED.store(true, Ordering::SeqCst);
    let mut buf = [0u8; PATTERN_LEN];
    let status = crate::sd::deluge_block_read(0, buf.as_mut_ptr(), TEST_SECTOR, 1);
    READ_STATUS_OK.store(status == 0, Ordering::SeqCst);
    READ_DATA_OK.store(buf == pattern(), Ordering::SeqCst);
    READ_DONE.store(true, Ordering::SeqCst);
}

/// Sector for the HIGH-priority-dispatch read — distinct from
/// [`TEST_SECTOR`] so the two phases' writes/reads can't collide within the
/// same backing image.
const TEST_SECTOR_PRIORITY: u32 = 4001;

static PRIO_READ_STARTED: AtomicBool = AtomicBool::new(false);
static PRIO_READ_DONE: AtomicBool = AtomicBool::new(false);
static PRIO_READ_STATUS_OK: AtomicBool = AtomicBool::new(false);
static PRIO_READ_DATA_OK: AtomicBool = AtomicBool::new(false);
static PRIO_READ_WAS_ON_FIBER: AtomicBool = AtomicBool::new(false);

fn pattern_priority() -> [u8; PATTERN_LEN] {
    let mut p = [0u8; PATTERN_LEN];
    for (i, b) in p.iter_mut().enumerate() {
        *b = (i as u8).wrapping_mul(53).wrapping_add(211);
    }
    p
}

/// HIGH-priority-dispatch sibling of [`read_op`]: same shape (runs on the fiber,
/// issues the real `deluge_block_read` C-ABI call), but reached via
/// `deluge_worker_run_priority` (see [`submit_pump_priority`]) instead of
/// `deluge_worker_run` — the HIGH-priority path `request_pump` actually
/// dispatches onto.
extern "C" fn priority_read_op(_ctx: *mut core::ffi::c_void) {
    PRIO_READ_WAS_ON_FIBER.store(crate::sd::deluge_storage_on_owner(), Ordering::SeqCst);
    PRIO_READ_STARTED.store(true, Ordering::SeqCst);
    let mut buf = [0u8; PATTERN_LEN];
    let status = crate::sd::deluge_block_read(0, buf.as_mut_ptr(), TEST_SECTOR_PRIORITY, 1);
    PRIO_READ_STATUS_OK.store(status == 0, Ordering::SeqCst);
    PRIO_READ_DATA_OK.store(buf == pattern_priority(), Ordering::SeqCst);
    PRIO_READ_DONE.store(true, Ordering::SeqCst);
}

/// Poll `cond` until true, sleeping in short increments; panics with `what` if
/// `deadline` passes first — the deadlock watchdog, same as
/// `owner_host_exercise.rs`'s.
fn wait_until(deadline: Instant, what: &str, mut cond: impl FnMut() -> bool) {
    loop {
        if cond() {
            return;
        }
        if Instant::now() >= deadline {
            panic!("sim_latency_host: timed out waiting for: {what}");
        }
        std::thread::sleep(Duration::from_millis(2));
    }
}

/// Runs the exercise. Panics (failing the caller, `#[test]`) on any assertion
/// failure or timeout.
pub fn run() {
    let _ = env_logger::builder().is_test(true).try_init();

    // Isolate this exercise's backing SD image: `deluge_bsp::sd`'s host shim
    // defaults to a fixed path shared across the whole process
    // (`$TMPDIR/deluge-bsp-sd.img`) — give this exercise its own file so it
    // can write a known pattern deterministically without depending on (or
    // clobbering) any other host sd test/exercise's image.
    let img = std::env::temp_dir().join(format!(
        "deluge-sim-latency-exercise-{}.img",
        std::process::id()
    ));
    // SAFETY: called before any other thread exists in this process (no
    // executor/spawned thread yet) — no concurrent env access.
    unsafe { std::env::set_var("DELUGE_SD_IMAGE", &img) };

    crate::sd::sim_latency::set_command_overhead_us(OVERHEAD_US);
    crate::sd::sim_latency::set_throughput_bytes_per_sec(THROUGHPUT_BYTES_PER_SEC);

    let (tx, rx) = mpsc::channel::<Job>();
    *SUBMIT_RX.lock().unwrap() = Some(rx);
    let (tx_priority, rx_priority) = mpsc::channel::<Job>();
    *SUBMIT_RX_PRIORITY.lock().unwrap() = Some(rx_priority);

    std::thread::Builder::new()
        .name("deluge-sim-latency-host".into())
        .spawn(|| {
            let executor: &'static mut Executor = Box::leak(Box::new(Executor::new()));
            executor.run(|spawner: Spawner| {
                spawner.spawn(worker_pump().unwrap());
                spawner.spawn(submit_pump().unwrap());
                spawner.spawn(submit_pump_priority().unwrap());
                spawner.spawn(counter_pump().unwrap());
                // The modeled-latency background task under test (see
                // `sd.rs`'s `sim_latency::pump` doc comment) — without this,
                // `sim_latency::delay`'s `REQUEST` signal has no consumer and
                // every modeled transfer hangs forever.
                spawner.spawn(crate::sd::sim_latency::pump().unwrap());
            });
        })
        .expect("spawning the host executor thread");

    // --- off-fiber write: seed a known pattern (also proves the off-fiber
    // `block_on(modeled_fut)` branch resolves under sim_latency, driven from
    // this — the driving, non-executor — thread) ---
    let pat = pattern();
    let write_start = Instant::now();
    let status = crate::sd::deluge_block_write(0, pat.as_ptr(), TEST_SECTOR, 1);
    let write_elapsed = write_start.elapsed();
    assert_eq!(
        status, 0,
        "off-fiber deluge_block_write failed: status={status}"
    );
    assert!(
        write_elapsed >= Duration::from_micros(OVERHEAD_US as u64),
        "off-fiber write returned in {write_elapsed:?}, faster than the modeled \
         {OVERHEAD_US}us command overhead — the modeled-latency wrapper isn't \
         being applied"
    );

    // --- the genuinely new assertion: an on-fiber read suspends the fiber
    // (yields to the executor) for roughly the modeled delay, rather than
    // blocking the process synchronously ---
    let counter_before = COUNTER.load(Ordering::SeqCst);
    let read_start = Instant::now();
    tx.send(read_op).expect("send read_op");

    let deadline = Instant::now() + Duration::from_secs(20);
    wait_until(deadline, "read_op to start", || {
        READ_STARTED.load(Ordering::SeqCst)
    });

    // Sample partway through the modeled delay: the read must not have
    // completed yet, and the independent counter task must have made
    // progress — proof the executor (and therefore other work) kept running
    // while the fiber was suspended, not blocked.
    std::thread::sleep(Duration::from_millis(u64::from(OVERHEAD_US) / 1000 / 2));
    assert!(
        !READ_DONE.load(Ordering::SeqCst),
        "read_op completed suspiciously fast (before half the modeled delay \
         elapsed) — the modeled latency isn't being honoured"
    );
    let counter_mid = COUNTER.load(Ordering::SeqCst);
    assert!(
        counter_mid > counter_before,
        "the independent counter task made no progress while read_op was \
         suspended (counter_before={counter_before}, counter_mid={counter_mid}) \
         — the fiber did not actually yield to the executor"
    );

    wait_until(deadline, "read_op to finish", || {
        READ_DONE.load(Ordering::SeqCst)
    });
    let read_elapsed = read_start.elapsed();

    // The counter must have kept advancing through the back half too.
    let counter_after = COUNTER.load(Ordering::SeqCst);
    assert!(
        counter_after > counter_mid,
        "the independent counter task stalled during the back half of the \
         suspension (counter_mid={counter_mid}, counter_after={counter_after})"
    );

    assert!(
        READ_WAS_ON_FIBER.load(Ordering::SeqCst),
        "read_op did not observe deluge_storage_on_owner() == true — this \
         exercise isn't actually testing the on-fiber block_on_fiber path"
    );
    assert!(
        READ_STATUS_OK.load(Ordering::SeqCst),
        "deluge_block_read returned a non-OK status"
    );
    assert!(
        READ_DATA_OK.load(Ordering::SeqCst),
        "deluge_block_read returned data that did not match the pattern \
         written earlier — the modeled-latency wrapper corrupted the \
         (unmodified) real file-image data path"
    );
    assert!(
        read_elapsed >= Duration::from_micros(OVERHEAD_US as u64),
        "on-fiber read completed in {read_elapsed:?}, faster than the modeled \
         {OVERHEAD_US}us command overhead — the modeled latency isn't being \
         honoured on the fiber path"
    );
    assert!(
        read_elapsed < Duration::from_secs(5),
        "on-fiber read took {read_elapsed:?} to resolve a {OVERHEAD_US}us \
         modeled delay — suggests it isn't being driven promptly by its Waker"
    );

    // --- the same suspend-and-yield proof, but dispatched via
    // `deluge_worker_run_priority` — the exact primitive `request_pump`'s real
    // off-owner path (`Coalescer{priority: true}` -> `Owner::run_priority`)
    // forwards to with no intervening logic (see the module doc's
    // "HIGH-priority dispatch" section). Proves the HIGH-priority queue path
    // and `sim_latency` suspension compose correctly, not just NORMAL
    // dispatch (above). ---
    let pat2 = pattern_priority();
    let write_start2 = Instant::now();
    let status2 = crate::sd::deluge_block_write(0, pat2.as_ptr(), TEST_SECTOR_PRIORITY, 1);
    let write_elapsed2 = write_start2.elapsed();
    assert_eq!(
        status2, 0,
        "Phase 2: off-fiber deluge_block_write failed: status={status2}"
    );
    assert!(
        write_elapsed2 >= Duration::from_micros(OVERHEAD_US as u64),
        "Phase 2: off-fiber write returned in {write_elapsed2:?}, faster than \
         the modeled {OVERHEAD_US}us command overhead"
    );

    let counter_before2 = COUNTER.load(Ordering::SeqCst);
    let read_start2 = Instant::now();
    tx_priority
        .send(priority_read_op)
        .expect("send priority_read_op");

    let deadline2 = Instant::now() + Duration::from_secs(20);
    wait_until(deadline2, "priority_read_op to start", || {
        PRIO_READ_STARTED.load(Ordering::SeqCst)
    });

    // Sample partway through the modeled delay: same suspend-and-yield check
    // as above, this time for the HIGH-priority-dispatched op.
    std::thread::sleep(Duration::from_millis(u64::from(OVERHEAD_US) / 1000 / 2));
    assert!(
        !PRIO_READ_DONE.load(Ordering::SeqCst),
        "Phase 2: priority_read_op completed suspiciously fast (before half \
         the modeled delay elapsed) — the modeled latency isn't being honoured \
         on the HIGH-priority dispatch path"
    );
    let counter_mid2 = COUNTER.load(Ordering::SeqCst);
    assert!(
        counter_mid2 > counter_before2,
        "Phase 2: the independent counter task made no progress while \
         priority_read_op was suspended (counter_before={counter_before2}, \
         counter_mid={counter_mid2}) — a HIGH-priority-dispatched op did not \
         actually yield the fiber to the executor"
    );

    wait_until(deadline2, "priority_read_op to finish", || {
        PRIO_READ_DONE.load(Ordering::SeqCst)
    });
    let read_elapsed2 = read_start2.elapsed();

    let counter_after2 = COUNTER.load(Ordering::SeqCst);
    assert!(
        counter_after2 > counter_mid2,
        "Phase 2: the independent counter task stalled during the back half \
         of the suspension (counter_mid={counter_mid2}, counter_after={counter_after2})"
    );
    assert!(
        PRIO_READ_WAS_ON_FIBER.load(Ordering::SeqCst),
        "Phase 2: priority_read_op did not observe deluge_storage_on_owner() \
         == true — the HIGH-priority dispatch did not actually reach the fiber"
    );
    assert!(
        PRIO_READ_STATUS_OK.load(Ordering::SeqCst),
        "Phase 2: deluge_block_read returned a non-OK status"
    );
    assert!(
        PRIO_READ_DATA_OK.load(Ordering::SeqCst),
        "Phase 2: deluge_block_read returned data that did not match the \
         pattern written earlier"
    );
    assert!(
        read_elapsed2 >= Duration::from_micros(OVERHEAD_US as u64),
        "Phase 2: HIGH-priority-dispatched read completed in {read_elapsed2:?}, \
         faster than the modeled {OVERHEAD_US}us command overhead"
    );
    assert!(
        read_elapsed2 < Duration::from_secs(5),
        "Phase 2: HIGH-priority-dispatched read took {read_elapsed2:?} to \
         resolve a {OVERHEAD_US}us modeled delay — suggests it isn't being \
         driven promptly by its Waker"
    );

    let _ = std::fs::remove_file(&img);
}
