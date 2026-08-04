//! `cargo test` entry point for the `sim_latency` suspending-host-SD-read
//! exercise. See `tests/support/sim_latency_host_exercise.rs` for the design.
//!
//! Requires the `sim_latency` feature:
//!   cargo test --features sim_latency --test sim_latency_host
#![cfg(all(not(target_os = "none"), feature = "sim_latency"))]
#![feature(impl_trait_in_assoc_type)]

#[path = "support/sim_latency_host_exercise.rs"]
mod exercise;
#[path = "../src/fiber.rs"]
mod fiber;
#[path = "../src/sd.rs"]
mod sd;

#[test]
fn sim_latency_host_exercise() {
    exercise::run();
}
