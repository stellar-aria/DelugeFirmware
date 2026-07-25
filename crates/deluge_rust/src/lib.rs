//! deluge_rust — umbrella staticlib for the Deluge Rust core.
//!
//! Links into the firmware / sim / conformance tests and re-exports the member
//! libraries' C ABIs so their `#[no_mangle]` symbols land in the final
//! `libdeluge_rust.a`:
//!   - `deluge_alloc`    — TLSF + slab allocator (`libdeluge/alloc.h`)
//!   - `deluge_resource` — cached-asset / resource manager (`deluge_resource.h`)
//!
//! It also provides the single bare-metal `#[panic_handler]` for the whole no_std
//! dependency graph (the member crates are plain rlibs with none of their own).

// `no_std` only for the bare-metal device build (target_os = "none"); on the host
// it's a normal std crate so the conformance staticlibs link with std's runtime.
#![cfg_attr(target_os = "none", no_std)]

// Re-export the members so their public items (incl. the `#[no_mangle] extern "C"`
// entry points) are reachable from this staticlib's root and thus retained.
pub use deluge_alloc::*;
pub use deluge_resource::*;

// SR3a Task 1: link-only force-link for the sim's `sim` feature (see Cargo.toml), mirroring
// `src/bsp/rust/src/main.rs`'s device `extern crate deluge_sample_source` (SR2d-5 Task 4).
// Unlike `deluge_alloc`/`deluge_resource` above, nothing in this crate's own Rust code
// references `deluge_sample_source`'s items -- there is no `pub use` to keep it live -- so
// without this, rustc/lld would never pull its single-object rlib into `libdeluge_rust.a`'s
// link at all, even with the optional dependency + `sim` feature wired in Cargo.toml, and the
// sim's C++ reader would keep resolving `sample_source.cpp`'s weak fallback.
#[cfg(feature = "sim")]
extern crate deluge_sample_source;

// Bare-metal panic handler (device only); the host build uses std's. This is the
// one panic handler for the entire dependency graph.
#[cfg(target_os = "none")]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    extern "C" {
        fn abort() -> !;
    }
    unsafe { abort() }
}
