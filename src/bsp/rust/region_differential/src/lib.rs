//! SR2c region-differential harness: drives a shared op sequence through TWO
//! region-port backings and asserts byte-identical regions.
//!
//! This is THE TOOL that gates SR2d (a native Rust region backing proven
//! byte-identical to the C++ one). This rung wires the C++ backing
//! (`cpp_backend`), validates the harness machinery, and proves the diff is
//! non-vacuous (`perturb`). `rust_backend` is SR2d's plug-in: a second
//! `ops::RegionPortOps` impl over `deluge_sample_source::cursor::SampleSource`,
//! proven byte-identical to the C++ backing over the same op sequences — the
//! C++ backend and the diff are unchanged.
pub mod cpp_backend;
pub mod diff;
pub mod gen;
pub mod ops;
pub mod perturb;
pub mod rust_backend;
