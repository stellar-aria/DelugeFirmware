//! Host entry point for `fill_logic`'s inline `#[cfg(test)]` unit tests (SR2d-4
//! Task 4): `begin`'s byte-range arithmetic is pure (no FFI, no statics), so
//! `src/fill_logic.rs` needs no fakes/doubles here — this file only exists to pull
//! the module into a `cfg(test)` build.
//!
//! `deluge-bsp-rust` is bin-only (no `[lib]` target — see `Cargo.toml`), so this
//! recompiles `src/fill_logic.rs` unmodified into this test binary via `#[path]`,
//! same convention as `tests/streaming_fill_host.rs`.
//! The actual `#[test]` functions live in `fill_logic.rs` itself (its own
//! `#[cfg(test)] mod tests`), not in this file.
#![cfg(not(target_os = "none"))]

#[path = "../src/fill_logic.rs"]
mod fill_logic;
