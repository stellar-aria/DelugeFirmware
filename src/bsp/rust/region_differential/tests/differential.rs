//! SR2c region-differential integration tests — the rung's gate.
//!
//! Two validating shapes:
//!   1. BASELINE MACHINERY: the C++ backend against a SECOND instance of itself
//!      over a generated op sequence — byte-identical (proves the driver,
//!      capture, and diff plumbing end to end).
//!   2. NON-VACUITY: one side wrapped in `Perturb` so a single captured
//!      `payload` byte / `resident_bytes` is corrupted — the diff MUST detect it
//!      at the right op/field. A C++-vs-C++ diff is trivially green, so this is
//!      what proves the harness has teeth as SR2d's gate.
use region_differential::cpp_backend::{CppBackend, Scenario};
use region_differential::diff::{capture, compare, OpResult};
use region_differential::gen::generate;
use region_differential::ops::Op;
use region_differential::perturb::{Perturb, Perturbation};
use region_differential::rust_backend::RustBackend;
use std::sync::Mutex;

// `deluge_sample_source` (SR2d's Rust region backing, wired in below as
// `RustBackend`) pulls in `deluge_resource`, whose `sync::Masked` calls out to
// three `extern "C"` critical-section primitives (`ENTER_CRITICAL_SECTION`,
// `EXIT_CRITICAL_SECTION`, `deluge_in_interrupt`). In production those are
// supplied by the BSP; `deluge_resource`'s own `cargo test` binary supplies
// host stubs internally, but that module is crate-private and not visible
// here — and unlike `deluge_sample_source`'s OWN unit tests (which link with
// `--cfg test` on that crate itself), THIS integration test binary links
// `deluge_sample_source`/`deluge_resource` as plain non-test rlibs, so their
// `#[cfg(test)]` stubs are compiled out of the artifact this binary pulls in.
// This binary must provide the three symbols itself, or the link fails with
// `undefined symbol: ENTER_CRITICAL_SECTION` (etc). Same pattern as
// `deluge_sample_source::host_critical_section_stubs`, minus the enter/exit
// counters nothing here asserts on. `deluge_in_interrupt` is hard-wired to
// `false` — the cursor's tests never model the audio-ISR context here — so
// masking is genuinely exercised, not bypassed (stubbing it to `true` would
// silently skip every critical section and weaken the gate).
//
// No duplicate-symbol risk from the C++ side: the three C++ TUs `build.rs`
// compiles (`sample_source.cpp`, `sample_source_test_support.cpp`,
// `harness_shim.cpp`) reference none of these symbol names.
mod host_critical_section_stubs {
    use core::cell::Cell;

    std::thread_local! {
        static DEPTH: Cell<u32> = const { Cell::new(0) };
        static TOKEN: Cell<Option<critical_section::RestoreState>> = const { Cell::new(None) };
    }

    #[unsafe(no_mangle)]
    extern "C" fn ENTER_CRITICAL_SECTION() {
        DEPTH.with(|d| {
            if d.get() == 0 {
                // SAFETY: released by the matching EXIT once this thread's depth hits 0.
                let t = unsafe { critical_section::acquire() };
                TOKEN.with(|tok| tok.set(Some(t)));
            }
            d.set(d.get() + 1);
        });
    }

    #[unsafe(no_mangle)]
    extern "C" fn EXIT_CRITICAL_SECTION() {
        let closed = DEPTH.with(|d| {
            let n = d.get().saturating_sub(1);
            d.set(n);
            n == 0
        });
        if closed {
            TOKEN.with(|tok| {
                if let Some(t) = tok.take() {
                    // SAFETY: stashed by this thread's outermost ENTER, above.
                    unsafe { critical_section::release(t) };
                }
            });
        }
    }

    // Single-threaded-per-test critical section; never models the audio-ISR
    // context, so "never in an interrupt" is the right constant answer here.
    #[unsafe(no_mangle)]
    extern "C" fn deluge_in_interrupt() -> bool {
        false
    }
}

/// The C++ backing uses PROCESS-WIDE singletons (`g_source_pool` in
/// `sample_source.cpp`, the process-wide lease table behind the fakes).
/// `cargo test` runs `#[test]`s in parallel threads, so any two that build a
/// `CppBackend` would race that shared state. Every test takes this lock for its
/// whole run — the two backends within a test are ALSO sequential (A captured
/// and dropped before B is opened), never live at once.
static TEST_LOCK: Mutex<()> = Mutex::new(());

/// Open a fresh C++ backend over `scenario`, drive `ops` through it, capture,
/// and drop it (releasing its pool slot + leases) before returning.
fn capture_cpp(scenario: &Scenario, ops: &[Op]) -> Vec<OpResult> {
    let mut backend = CppBackend::open(scenario);
    capture(&mut backend, ops)
}

/// Same, but the backend is wrapped in `Perturb` — the stand-in for a divergent
/// SR2d Rust backing.
fn capture_perturbed(scenario: &Scenario, ops: &[Op], p: Perturbation) -> Vec<OpResult> {
    let mut backend = Perturb::new(CppBackend::open(scenario), p);
    capture(&mut backend, ops)
}

/// Open a fresh Rust backend (`SampleSource<HarnessResidency>`) over `scenario`,
/// drive `ops` through it, capture, and drop it. Unlike `CppBackend`, `RustBackend`
/// has no process-wide singleton state, so it does not itself need `TEST_LOCK` —
/// but every call site here is made under the same lock as its paired
/// `capture_cpp` call, since the two backends in a test run sequentially anyway.
fn capture_rust(scenario: &Scenario, ops: &[Op]) -> Vec<OpResult> {
    let mut backend = RustBackend::open(scenario);
    capture(&mut backend, ops)
}

// ---------------------------------------------------------------------------
// 1. Baseline machinery — C++ vs C++ is byte-identical.
// ---------------------------------------------------------------------------

/// Dense, all-loaded stream with a short last cluster (so `resident_bytes`
/// varies across the sequence). C++ vs C++ over a generated op sequence.
#[test]
fn baseline_identical_dense() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario::dense(5, 68); // 4 full clusters + a 4-byte tail
    let ops = generate(0xC0FFEE, scenario.num_clusters, 200);

    let a = capture_cpp(&scenario, &ops);
    let b = capture_cpp(&scenario, &ops);
    compare(&a, &b).expect("baseline C++-vs-C++ must be byte-identical");
}

/// A stream mixing loaded, not-yet-loaded (→ LOADING) and un-reservable
/// (→ UNAVAILABLE) clusters, so the diff walks all three residency states plus
/// retain/release lease effects. Still C++ vs C++ → identical.
#[test]
fn baseline_identical_mixed_states() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario {
        num_clusters: 6,
        audio_data_length_bytes: 6 * 16,
        unloaded: vec![3],      // acquire(3) → LOADING
        unavailable: vec![5],   // acquire(5) → UNAVAILABLE
    };
    let ops = generate(0x5EED, scenario.num_clusters, 200);

    let a = capture_cpp(&scenario, &ops);
    let b = capture_cpp(&scenario, &ops);
    compare(&a, &b).expect("baseline (mixed states) C++-vs-C++ must be byte-identical");
}

/// Different seeds must all stay identical C++-vs-C++ (the diff never emits a
/// false positive on the plumbing).
#[test]
fn baseline_identical_many_seeds() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario::dense(8, 8 * 16 - 5);
    for seed in [1u64, 2, 3, 42, 1000, 999_999] {
        let ops = generate(seed, scenario.num_clusters, 150);
        let a = capture_cpp(&scenario, &ops);
        let b = capture_cpp(&scenario, &ops);
        compare(&a, &b).unwrap_or_else(|d| panic!("seed {seed}: {d}"));
    }
}

// ---------------------------------------------------------------------------
// 2. Non-vacuity — the diff DETECTS an injected divergence.
// ---------------------------------------------------------------------------

/// Control for the non-vacuity tests: a `Perturb` wrapper whose trigger never
/// fires (ordinal past every Ready acquire) mutates nothing, so the diff is
/// still Ok. Proves the divergence tests below fail because of the INJECTED
/// change — not because wrapping a backend in `Perturb` differs on its own.
#[test]
fn perturb_that_never_fires_is_identical() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario::dense(5, 68);
    let ops = generate(0xC0FFEE, scenario.num_clusters, 200);

    let clean = capture_cpp(&scenario, &ops);
    let wrapped = capture_perturbed(
        &scenario,
        &ops,
        Perturbation::FlipPayloadByte { on_ready_ordinal: 100_000, byte: 0 },
    );
    compare(&clean, &wrapped).expect("a never-firing Perturb must not diverge");
}

/// Corrupt one payload byte on the first Ready acquire; the diff MUST flag a
/// `payload` divergence. This is the load-bearing proof the harness has teeth.
#[test]
fn divergence_detected_payload_byte() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario::dense(5, 68);
    let ops = generate(0xC0FFEE, scenario.num_clusters, 200);

    let clean = capture_cpp(&scenario, &ops);
    let dirty = capture_perturbed(
        &scenario,
        &ops,
        Perturbation::FlipPayloadByte { on_ready_ordinal: 0, byte: 3 },
    );

    let div = compare(&clean, &dirty).expect_err("a flipped payload byte MUST be detected");
    assert_eq!(div.field, "payload", "divergence should be on the payload field: {div}");
    // The first Ready acquire in this sequence is op #0 (Acquire{index:0,...}).
    assert_eq!(div.op_index, 0, "should point at the first (perturbed) op: {div}");
}

/// Corrupt `resident_bytes` on the first Ready acquire; the diff MUST flag a
/// `resident_bytes` divergence (the other field the brief names).
#[test]
fn divergence_detected_resident_bytes() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario::dense(5, 68);
    let ops = generate(0xC0FFEE, scenario.num_clusters, 200);

    let clean = capture_cpp(&scenario, &ops);
    let dirty = capture_perturbed(
        &scenario,
        &ops,
        Perturbation::BumpResidentBytes { on_ready_ordinal: 0 },
    );

    let div = compare(&clean, &dirty).expect_err("a bumped resident_bytes MUST be detected");
    assert_eq!(div.field, "resident_bytes", "divergence should be on resident_bytes: {div}");
    assert_eq!(div.op_index, 0, "should point at the first (perturbed) op: {div}");
}

/// A perturbation on a LATER Ready acquire must be caught at THAT op, not op 0 —
/// proving the diff localizes divergence rather than just detecting "differ
/// somewhere".
#[test]
fn divergence_localized_to_the_right_op() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario::dense(5, 68);
    let ops = generate(0xC0FFEE, scenario.num_clusters, 200);

    let clean = capture_cpp(&scenario, &ops);
    // Corrupt the 4th Ready acquire (ordinal 3).
    let dirty = capture_perturbed(
        &scenario,
        &ops,
        Perturbation::FlipPayloadByte { on_ready_ordinal: 3, byte: 0 },
    );

    let div = compare(&clean, &dirty).expect_err("later-op perturbation MUST be detected");
    assert_eq!(div.field, "payload");
    // Locate the op index of the 4th Ready acquire in `clean` to check the diff
    // reported exactly there.
    let ready_ops: Vec<usize> = clean
        .iter()
        .enumerate()
        .filter(|(_, r)| r.region.is_some())
        .map(|(i, _)| i)
        .collect();
    assert_eq!(div.op_index, ready_ops[3], "divergence not localized to the perturbed op: {div}");
}

// ---------------------------------------------------------------------------
// 3. THE GATE — C++ vs SR2d's Rust backing is byte-identical.
// ---------------------------------------------------------------------------

/// Dense, all-loaded stream with a short last cluster. C++ vs Rust over a
/// generated op sequence must be byte-identical.
#[test]
fn cpp_and_rust_backends_are_byte_identical_dense() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario::dense(5, 68); // 4 full clusters + a 4-byte tail
    let ops = generate(0xC0FFEE, scenario.num_clusters, 200);

    let a = capture_cpp(&scenario, &ops);
    let b = capture_rust(&scenario, &ops);
    compare(&a, &b).unwrap_or_else(|d| panic!("C++ vs Rust diverged: {d}"));
}

/// A stream mixing loaded, not-yet-loaded (→ LOADING) and un-reservable
/// (→ UNAVAILABLE) clusters — C++ vs Rust across all three residency states plus
/// retain/release lease effects.
#[test]
fn cpp_and_rust_backends_are_byte_identical_with_loading_and_unavailable() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario {
        num_clusters: 7,
        audio_data_length_bytes: 7 * 16,
        unloaded: vec![3],    // acquire(3) → LOADING
        unavailable: vec![5], // acquire(5) → UNAVAILABLE
    };
    let ops = generate(0x5EED, scenario.num_clusters, 200);

    let a = capture_cpp(&scenario, &ops);
    let b = capture_rust(&scenario, &ops);
    compare(&a, &b).unwrap_or_else(|d| panic!("C++ vs Rust diverged: {d}"));
}

/// Multiple seeds over a dense scenario must all stay identical C++-vs-Rust —
/// the gate is not a lucky single-seed coincidence.
#[test]
fn cpp_and_rust_byte_identical_multi_seed_sweep() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let scenario = Scenario::dense(8, 8 * 16 - 5);
    for seed in [1u64, 2, 7, 42, 0xDEAD, 0xBEEF] {
        let ops = generate(seed, scenario.num_clusters, 150);
        let a = capture_cpp(&scenario, &ops);
        let b = capture_rust(&scenario, &ops);
        compare(&a, &b).unwrap_or_else(|d| panic!("seed {seed:#x} diverged: {d}"));
    }
}
