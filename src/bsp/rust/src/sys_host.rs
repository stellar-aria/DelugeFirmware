//! Host stand-in for the device's `mod sys` (the bindgen output from the C++
//! app headers, which build.rs only generates for the arm-eabi target). These
//! types reproduce the C-ABI layout of `include/libdeluge/*.h` exactly, so the
//! same boundary code in `control.rs`/`display.rs` compiles and runs unchanged
//! on the host. Layout is checked by the `control.rs` ABI guard
//! (`DelugeInputEvent` == 8 bytes).
//!
//! Verified field-by-field against `include/libdeluge/types.h`
//! (`DelugeStatus`) and `include/libdeluge/control_surface.h`
//! (`DelugeColour`, `DelugeInputEventKind`, `DelugeInputEvent`,
//! `DelugeBootInfo`).
#![allow(non_camel_case_types, non_upper_case_globals, dead_code)]

/// `include/libdeluge/types.h`: `DelugeStatus`. Values span -13..=0; the C++
/// build uses plain (unfixed) `int`-sized enums throughout (no
/// `-fshort-enums` anywhere — see `../build.rs`), so this is a 4-byte enum.
pub type DelugeStatus = i32;
pub const DelugeStatus_DELUGE_OK: DelugeStatus = 0;

/// `include/libdeluge/control_surface.h`: `DelugeInputEventKind`. PAD=0,
/// BUTTON=1, ENCODER=2 (unused by control.rs — encoder motion arrives via
/// `deluge_encoder_take_edges`, not the event queue), NO_PRESSES=3. All
/// values non-negative, so clang's default (unfixed) enum picks `unsigned
/// int` — 4 bytes, same as every other libdeluge enum (see `../build.rs`).
pub type DelugeInputEventKind = u32;
pub const DelugeInputEventKind_DELUGE_EVENT_PAD: DelugeInputEventKind = 0;
pub const DelugeInputEventKind_DELUGE_EVENT_BUTTON: DelugeInputEventKind = 1;
pub const DelugeInputEventKind_DELUGE_EVENT_ENCODER: DelugeInputEventKind = 2;
pub const DelugeInputEventKind_DELUGE_EVENT_NO_PRESSES: DelugeInputEventKind = 3;

/// `include/libdeluge/control_surface.h`: `DelugeInputEvent`. `#[repr(C)]`
/// with no manual padding needed: `value`'s `i16` alignment only needs 2
/// bytes, so it abuts `y` directly — Rust's C-compatible layout algorithm
/// reproduces the C struct exactly (kind@0 4B, x@4, y@5, value@6), 8 bytes
/// total (see the ABI guard in `control.rs`, which asserts this at compile
/// time).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DelugeInputEvent {
    pub kind: DelugeInputEventKind, // offset 0 (4 bytes)
    pub x: u8,                      // offset 4
    pub y: u8,                      // offset 5
    pub value: i16,                 // offset 6
} // size: 8 bytes

/// `include/libdeluge/control_surface.h`: `DelugeColour`. `#[repr(C)]`, three
/// packed `u8` channels with no padding.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DelugeColour {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

/// `include/libdeluge/control_surface.h`: `DelugeBootInfo`.
#[repr(C)]
pub struct DelugeBootInfo {
    pub pic_firmware_version: u8,
    pub oled_present: bool,
    pub factory_reset_requested: bool,
}
