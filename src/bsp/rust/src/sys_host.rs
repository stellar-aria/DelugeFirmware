//! Host stand-in for the device's `mod sys` (the bindgen output from the C++
//! app headers, which build.rs only generates for the arm-eabi target). These
//! types reproduce the C-ABI layout of `include/libdeluge/*.h` exactly, so the
//! same boundary code in `control.rs`/`display.rs` compiles and runs unchanged
//! on the host. Layout is checked by the `control.rs` ABI guard
//! (`DelugeInputEvent` == 6 bytes).
//!
//! Verified field-by-field against `include/libdeluge/types.h`
//! (`DelugeStatus`) and `include/libdeluge/control_surface.h`
//! (`DelugeColour`, `DelugeInputEventKind`, `DelugeInputEvent`,
//! `DelugeBootInfo`).
#![allow(non_camel_case_types, non_upper_case_globals, dead_code)]

/// `include/libdeluge/types.h`: `DelugeStatus`. Values span -13..=0, so under
/// the device's `-fshort-enums` build this is a 1-byte enum.
pub type DelugeStatus = i8;
pub const DelugeStatus_DELUGE_OK: DelugeStatus = 0;

/// `include/libdeluge/control_surface.h`: `DelugeInputEventKind`. PAD=0,
/// BUTTON=1, ENCODER=2 (unused by control.rs — encoder motion arrives via
/// `deluge_encoder_take_edges`, not the event queue), NO_PRESSES=3.
pub type DelugeInputEventKind = u8;
pub const DelugeInputEventKind_DELUGE_EVENT_PAD: DelugeInputEventKind = 0;
pub const DelugeInputEventKind_DELUGE_EVENT_BUTTON: DelugeInputEventKind = 1;
pub const DelugeInputEventKind_DELUGE_EVENT_ENCODER: DelugeInputEventKind = 2;
pub const DelugeInputEventKind_DELUGE_EVENT_NO_PRESSES: DelugeInputEventKind = 3;

/// `include/libdeluge/control_surface.h`: `DelugeInputEvent`. `#[repr(C)]`
/// with an explicit 1-byte padding slot before `value` for its natural `i16`
/// alignment, giving 6 bytes total — matching the C struct exactly.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DelugeInputEvent {
    pub kind: DelugeInputEventKind, // offset 0 (1 byte)
    pub x: u8,                      // offset 1
    pub y: u8,                      // offset 2
    // offset 3: padding (natural i16 alignment)
    pub value: i16, // offset 4
} // size: 6 bytes

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
