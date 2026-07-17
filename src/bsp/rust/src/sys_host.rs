//! Host stand-in for the device `mod sys` (bindgen output from the C++ app
//! headers, which build.rs only generates on the arm-eabi target). These
//! reproduce the C-ABI layout of `include/libdeluge/*.h` exactly so the same
//! boundary code in `control.rs`/`display.rs` compiles and runs on the host.
//! Layout is asserted by the `control.rs` ABI guard (DelugeInputEvent == 6 B).
//!
//! Verified against `include/libdeluge/types.h` (DelugeStatus) and
//! `include/libdeluge/control_surface.h` (DelugeColour, DelugeInputEventKind,
//! DelugeInputEvent, DelugeBootInfo) — see `.superpowers/sdd/task-4-report.md`
//! for the field-by-field cross-check.
#![allow(non_camel_case_types, non_upper_case_globals, dead_code)]

/// include/libdeluge/types.h: DelugeStatus. Values span -13..=0, so under the
/// device's `-fshort-enums` build this is a 1-byte enum.
pub type DelugeStatus = i8;
pub const DelugeStatus_DELUGE_OK: DelugeStatus = 0;

/// include/libdeluge/control_surface.h: DelugeInputEventKind. PAD=0, BUTTON=1,
/// ENCODER=2 (not used by control.rs — encoder motion arrives via
/// deluge_encoder_take_edges, not the event queue), NO_PRESSES=3.
pub type DelugeInputEventKind = u8;
pub const DelugeInputEventKind_DELUGE_EVENT_PAD: DelugeInputEventKind = 0;
pub const DelugeInputEventKind_DELUGE_EVENT_BUTTON: DelugeInputEventKind = 1;
pub const DelugeInputEventKind_DELUGE_EVENT_ENCODER: DelugeInputEventKind = 2;
pub const DelugeInputEventKind_DELUGE_EVENT_NO_PRESSES: DelugeInputEventKind = 3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DelugeInputEvent {
    pub kind: DelugeInputEventKind, // @0 (1 byte)
    pub x: u8,                      // @1
    pub y: u8,                      // @2
    // @3 padding (natural i16 alignment)
    pub value: i16, // @4
} // size 6

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DelugeColour {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

#[repr(C)]
pub struct DelugeBootInfo {
    pub pic_firmware_version: u8,
    pub oled_present: bool,
    pub factory_reset_requested: bool,
}
