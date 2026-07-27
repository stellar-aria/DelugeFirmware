//! deluge_sample_reader — the sample range-reader C-ABI (`include/libdeluge/sample_reader.h`, U1).
//!
//! A zero-copy streaming reader-handle (plus, later, a stateless copy convenience) over a sample's
//! source residency, for non-voice consumers that read raw-PCM frame RANGES instead of reaching
//! `StreamedChunk` internals the way they do today — the non-voice twin of the voice region port
//! (`deluge_sample_source`'s `DelugeSampleSource`/`DelugeSampleRegion`). Coexists with the existing
//! facade (`peek`/`prefetch`/`load_now`/`request`/`dequeue`); no consumer migrates in U1 (that is
//! U2) — see the design doc this crate's Cargo.toml references.
//!
//! Task 1 (this landing) is lifecycle-only: `open`/`seek`/`close`. `window`/`advance`/`ok` and the
//! stateless `deluge_sample_read` copy — the actual streaming reads — land in later tasks; their
//! signatures already exist in the header (the header is the contract), just not yet backed here.
#![no_std]

extern crate alloc;

pub mod abi;
pub mod reader;

// `deluge_resource::sync` calls out to three C-ABI critical-section primitives that, in the real
// firmware/host-sim link, the BSP (`src/bsp/rust/src/services.rs`) provides. `deluge_resource`'s
// own `cargo test` binary supplies host stubs for them internally (`sync::stubs`,
// `#[cfg(test)]`-gated and crate-private), but that module isn't visible to downstream crates — so
// this crate's own test binary, which links `deluge_resource` as a plain (non-test) rlib, must
// provide the same three symbols itself or every test that drops a `Lease` (which goes through
// `Masked`) fails to link. Copied verbatim from `deluge_sample_source::lib`'s own
// `host_critical_section_stubs` — the identical problem, the identical fix.
#[cfg(test)]
mod host_critical_section_stubs {
    extern crate std;
    use core::cell::Cell;

    std::thread_local! {
        static DEPTH: Cell<u32> = const { Cell::new(0) };
        static TOKEN: Cell<Option<critical_section::RestoreState>> = const { Cell::new(None) };
    }

    #[unsafe(no_mangle)]
    extern "C" fn ENTER_CRITICAL_SECTION() {
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
        let closed = DEPTH.with(|d| {
            let n = d.get().saturating_sub(1);
            d.set(n);
            n == 0
        });
        if closed {
            TOKEN.with(|tok| {
                if let Some(t) = tok.take() {
                    // SAFETY: stashed by this thread's outermost ENTER, above.
                    unsafe { critical_section::release(t) };
                }
            });
        }
    }

    // This crate's tests are single-threaded and never model the audio-ISR context (that
    // asymmetry is `deluge_resource`'s own concern, proved in its crate) — so "never in an
    // interrupt" is the right constant answer here.
    #[unsafe(no_mangle)]
    extern "C" fn deluge_in_interrupt() -> bool {
        false
    }
}
