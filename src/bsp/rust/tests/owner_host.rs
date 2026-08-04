//! Drive the REAL `deluge::storage::Owner::run` dispatch seam
//! (`fiber.rs`'s `deluge_worker_run`/`worker_poll` C-ABI, plus `sd.rs`'s
//! `deluge_storage_on_owner`) on a host `platform-std` Embassy executor.
//!
//! The normal (unsanitized) `cargo test` entry point. The exercise body is
//! shared with the ThreadSanitizer entry point (`examples/owner_host_tsan.rs`)
//! via `tests/support/owner_host_exercise.rs` — see that file's header for the
//! exercise design and why two entry points exist. See `HOST_HARNESS.md` for
//! the TSan invocation.
#![cfg(not(target_os = "none"))]
#![feature(impl_trait_in_assoc_type)]

#[path = "support/owner_host_exercise.rs"]
mod exercise;
#[path = "../src/fiber.rs"]
mod fiber;
#[path = "../src/sd.rs"]
mod sd;

#[test]
fn owner_host_exercise() {
    exercise::run();
}
