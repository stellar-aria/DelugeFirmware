//! ThreadSanitizer entry point for the Owner-dispatch exercise.
//!
//! A plain `fn main()` binary, NOT a `#[test]` — see
//! `examples/scheduler_host_tsan.rs`'s header (and `HOST_HARNESS.md`) for why:
//! `cargo test`'s `--test` harness needs `libtest`, and building `libtest` for
//! a custom `-Zbuild-std` sanitized target hits a reproducible nightly Cargo
//! bug. Run via `cargo run --example owner_host_tsan`; see `HOST_HARNESS.md`
//! for the exact TSan invocation (custom target JSON +
//! `-Zbuild-std=core,alloc,std,panic_abort`).
//!
//! Shares the actual exercise body with the normal `cargo test` entry point
//! (`tests/owner_host.rs`) via `tests/support/owner_host_exercise.rs` — see
//! that file's header for the full design rationale.
#![cfg(not(target_os = "none"))]
#![feature(impl_trait_in_assoc_type)]

#[path = "../tests/support/owner_host_exercise.rs"]
mod exercise;
#[path = "../src/fiber.rs"]
mod fiber;
#[path = "../src/sd.rs"]
mod sd;

fn main() {
    exercise::run();
    println!("owner_host_tsan: PASSED");
}
