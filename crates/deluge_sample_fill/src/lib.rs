//! The shared cluster-fill core (C2a). See Cargo.toml's header for the crate's role.
//! Task 1 lands `fill_logic`; the fill-context table (Task 2) and the `native_fill`-gated
//! sync fill (Task 3) are added below in later tasks.
#![no_std]

pub mod fill_logic;
