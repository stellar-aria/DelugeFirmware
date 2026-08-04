//! B1 synchronization primitives for the Manager. Every access to the Manager's
//! shared `Cell` state routes through [`m_get`] / [`m_set`] / [`m_rmw`], which
//! wrap the access in an asymmetric critical section (`Masked`): the fiber masks
//! the minimal O(1) window; the audio ISR skips the mask entirely (gated on
//! `!deluge_in_interrupt()`) because it can never be preempted by the fiber, so
//! its RMWs are already atomic w.r.t. it. See docs/dev/known-concurrency-bugs.md
//! (B1) and docs/superpowers/specs/2026-07-19-manager-b1-sync-design.md.
//!
//! On host — where the preemptive TSan harness runs audio on a real second OS
//! thread and `deluge_in_interrupt()` is always false — BOTH threads mask, so
//! the global critical-section mutex serializes them and TSan sees no race.

use core::cell::Cell;

// The C-ABI critical-section primitives, provided by the BSP (services.rs) in
// the real device/host_app link. In the crate's own `cargo test` binary they
// are provided by the `#[cfg(test)]` stubs at the bottom of this file.
unsafe extern "C" {
    fn ENTER_CRITICAL_SECTION();
    fn EXIT_CRITICAL_SECTION();
    fn deluge_in_interrupt() -> bool;
}

/// RAII asymmetric critical section. Enters iff called outside the audio ISR
/// (`!deluge_in_interrupt()`); a no-op on the audio path (already atomic vs the
/// fiber) and near-no-op on the cooperative BSP (nothing to preempt).
pub struct Masked {
    active: bool,
}

impl Masked {
    #[inline]
    pub fn enter() -> Self {
        // SAFETY: FFI to the BSP interrupt-mask primitives; no invariants beyond
        // ENTER/EXIT being balanced, which the Drop impl guarantees.
        let active = unsafe { !deluge_in_interrupt() };
        if active {
            unsafe { ENTER_CRITICAL_SECTION() };
        }
        Masked { active }
    }
}

impl Drop for Masked {
    #[inline]
    fn drop(&mut self) {
        if self.active {
            // SAFETY: balanced with the ENTER in `enter`.
            unsafe { EXIT_CRITICAL_SECTION() };
        }
    }
}

/// Masked coherent read of a single cell. The mask is released on return, so a
/// scan that calls this per slot holds the mask for only one slot at a time.
#[inline]
pub fn m_get<T: Copy>(c: &Cell<T>) -> T {
    let _m = Masked::enter();
    c.get()
}

/// Masked write of a single cell.
#[inline]
pub fn m_set<T>(c: &Cell<T>, v: T) {
    let _m = Masked::enter();
    c.set(v);
}

/// Masked get→modify→set window over a single cell. The whole window is
/// indivisible w.r.t. the fiber (and atomic on the audio path).
#[inline]
pub fn m_rmw<T: Copy, R>(c: &Cell<T>, f: impl FnOnce(&mut T) -> R) -> R {
    let _m = Masked::enter();
    let mut v = c.get();
    let r = f(&mut v);
    c.set(v);
    r
}

// ---- test-only providers of the C-ABI primitives -----------------------------
// In `cargo test -p deluge_resource` there is no BSP in the link, so provide the
// three extern symbols here, backed by the real `critical-section` std impl (so
// the guard is exercised for real), plus a settable in-interrupt flag and
// ENTER/EXIT counters so the asymmetry is directly assertable.
//
// The ENTER/EXIT counters are thread-local (not global atomics): `cargo test`
// runs test functions on multiple threads in parallel, and a global counter
// would let one test's m_rmw calls pollute another concurrently-running test's
// enter/exit-count assertions. Thread-local counters make each test's counts
// depend only on that test's own thread, matching IN_ISR/DEPTH/TOKEN below.
// `pub(crate)` (not private) so sibling modules' tests — notably `facade`'s
// `Lease::drop` ISR-safety proof — can drive the same two-context model
// (`deluge_in_interrupt_set`) and read the same critical-section counters this
// module's own tests use, rather than re-deriving a second harness.
#[cfg(test)]
pub(crate) mod stubs {
    use core::cell::Cell;

    std::thread_local! {
        static IN_ISR: Cell<bool> = const { Cell::new(false) };
        static DEPTH: Cell<u32> = const { Cell::new(0) };
        static TOKEN: Cell<Option<critical_section::RestoreState>> = const { Cell::new(None) };
        static ENTER_COUNT: Cell<u64> = const { Cell::new(0) };
        static EXIT_COUNT: Cell<u64> = const { Cell::new(0) };
    }

    #[unsafe(no_mangle)]
    extern "C" fn ENTER_CRITICAL_SECTION() {
        ENTER_COUNT.with(|c| c.set(c.get() + 1));
        DEPTH.with(|d| {
            if d.get() == 0 {
                // SAFETY: released by the matching EXIT once this thread's depth hits 0.
                let t = unsafe { critical_section::acquire() };
                TOKEN.with(|tok| tok.set(Some(t)));
            }
            d.set(d.get() + 1);
        });
    }

    #[unsafe(no_mangle)]
    extern "C" fn EXIT_CRITICAL_SECTION() {
        EXIT_COUNT.with(|c| c.set(c.get() + 1));
        let closed = DEPTH.with(|d| {
            let n = d.get().saturating_sub(1);
            d.set(n);
            n == 0
        });
        if closed {
            TOKEN.with(|tok| {
                if let Some(t) = tok.take() {
                    // SAFETY: stashed by this thread's outermost ENTER.
                    unsafe { critical_section::release(t) };
                }
            });
        }
    }

    #[unsafe(no_mangle)]
    extern "C" fn deluge_in_interrupt() -> bool {
        IN_ISR.with(|f| f.get())
    }

    pub fn deluge_in_interrupt_set(v: bool) {
        IN_ISR.with(|f| f.set(v));
    }
    pub fn cs_enter_count() -> u64 {
        ENTER_COUNT.with(|c| c.get())
    }
    pub fn cs_exit_count() -> u64 {
        EXIT_COUNT.with(|c| c.get())
    }
    pub fn cs_reset_counts() {
        ENTER_COUNT.with(|c| c.set(0));
        EXIT_COUNT.with(|c| c.set(0));
    }
}

#[cfg(test)]
mod tests {
    use super::stubs::*;
    use super::*;

    #[test]
    fn fiber_masks_rmw() {
        cs_reset_counts();
        deluge_in_interrupt_set(false); // fiber context
        let c = Cell::new(1u32);
        m_rmw(&c, |v| *v += 1);
        assert_eq!(c.get(), 2);
        assert_eq!(cs_enter_count(), 1, "fiber RMW must mask exactly once");
        assert_eq!(cs_exit_count(), 1);
    }

    #[test]
    fn audio_skips_mask() {
        cs_reset_counts();
        deluge_in_interrupt_set(true); // audio ISR context
        let c = Cell::new(1u32);
        m_rmw(&c, |v| *v += 1);
        assert_eq!(c.get(), 2);
        assert_eq!(cs_enter_count(), 0, "audio path must not mask");
        assert_eq!(cs_exit_count(), 0);
        deluge_in_interrupt_set(false); // restore for other tests on this thread
    }

    #[test]
    fn nesting_masks_once_per_level() {
        cs_reset_counts();
        deluge_in_interrupt_set(false);
        let outer = Cell::new(0u32);
        let inner = Cell::new(0u32);
        m_rmw(&outer, |o| {
            *o += 1;
            m_rmw(&inner, |i| *i += 1); // nested masked access
        });
        assert_eq!(outer.get(), 1);
        assert_eq!(inner.get(), 1);
        assert_eq!(cs_enter_count(), 2, "one enter per masked call, nested");
        assert_eq!(cs_exit_count(), 2);
    }
}
