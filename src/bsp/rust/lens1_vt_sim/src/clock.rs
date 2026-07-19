//! [`PeekableMockDriver`]: the discrete-event virtual clock. Identical shape to
//! Spike A's (`../../spike_mock_clock/src/main.rs`) — see that file's doc
//! comment for the full rationale (why not `embassy_time::MockDriver`, why this
//! needs a deadline-peek method MockDriver lacks). Copied rather than shared as
//! a library: the spike is deliberately throwaway, this is the real harness: a
//! second, reviewed copy costs nothing and keeps this package fully
//! self-contained.
use core::cell::RefCell;
use core::task::Waker;

use critical_section::Mutex as CsMutex;
use embassy_time_queue_utils::Queue;

pub struct PeekableMockDriver(CsMutex<RefCell<Inner>>);

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
    pub fn get() -> &'static Self {
        &DRIVER
    }

    /// Jump virtual time forward by `ticks`, synchronously firing (waking)
    /// every timer whose deadline is now `<= now`.
    pub fn advance_ticks(&self, ticks: u64) {
        critical_section::with(|cs| {
            let mut inner = self.0.borrow_ref_mut(cs);
            inner.now += ticks;
            let now = inner.now;
            inner.queue.next_expiration(now);
        });
    }

    /// The next scheduled wake tick, or `None` if no task has an outstanding
    /// timer at all. Only safe to trust at a quiescence point (see Spike A's
    /// identical struct doc for why this is sound).
    pub fn peek_next_deadline(&self) -> Option<u64> {
        critical_section::with(|cs| {
            let mut inner = self.0.borrow_ref_mut(cs);
            let now = inner.now;
            let next = inner.queue.next_expiration(now);
            (next != u64::MAX).then_some(next)
        })
    }
}
