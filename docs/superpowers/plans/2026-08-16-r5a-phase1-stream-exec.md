# STREAM_EXEC — promoting the streaming fill task off the thread executor

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `streaming_fill_task` off the cooperative thread executor onto a
higher-priority preemptive executor, so that a future non-yielding `block_on` in thread
mode can no longer monopolise the context that releases the filesystem mutex.

**Architecture:** On device, a second `InterruptExecutor` (`STREAM_EXEC`) driven by GIC
SGI 9 at a priority numerically between the audio SGI (20) and thread mode, so audio still
preempts streaming and streaming preempts the worker fiber. On the Lens 1 virtual-time sim,
the equivalent is `HP_EXEC`, the executor the in-spin progress hook already pumps. On the
threaded host BSP, a third OS thread with its own executor. Before any of that, one
correctness prerequisite: the resource manager's asymmetric critical section currently
assumes exactly two contexts, and a third preemptive context breaks it.

**Tech Stack:** Rust (embassy-executor with the A9 `InterruptExecutor` fork,
embassy-sync), `rza1l-hal` GIC, C++23 for the C-ABI boundary headers, CppSpec + cargo test,
`./dbt` for builds and harnesses.

**Spec:** `docs/superpowers/specs/2026-08-07-r5a-fiber-io-retirement-design.md` §4 Phase 1
(and §2 for why this is a hard prerequisite rather than a tuning step). Ladder context:
`docs/superpowers/specs/2026-08-08-storage-execution-model-end-state-design.md` §5.

## Global Constraints

- **Branch first.** All work on a new branch off `next`. Never commit to `main`.
- **Never `git commit --amend`.** The clang-format/rustfmt pre-commit hooks reformat files
  and fail the commit; recover with a **fresh** commit, never an amend.
- **No `Co-Authored-By` and no "Generated with Claude" trailers** in any commit message.
- **No process labels in commit subjects** — no "Task N", "Step N", "Phase N", "rung".
  Describe the change itself.
- **Never pass `update` to any golden script.** `scripts/golden_embassy_diff.sh` is run with
  `check` only. Re-baselining requires explicit authorization that this plan does not grant.
- **New Rust is idiomatic; new C is minimal C-ABI.** Behaviour-preserving edits stay faithful.
- **Device build is `cargo device --release` from `src/bsp/rust`** — `./dbt rust` is an
  x86-64 host build and does **not** prove the device links.
- **Firmware C++ build is `./dbt build Debug`**, unit tests `./dbt test`, harnesses
  `./dbt harness run <name>`. Never run cargo by hand inside a harness directory.
- **Exact GIC values, copied from `src/bsp/rust/src/main.rs`:** `AUDIO_SGI = 8`,
  `AUDIO_SGI_PRIORITY = 20`, OSTM = 14, UART/MIDI = 10, DMAC = 13, PMR = 31. Lower number =
  more urgent. This plan introduces `STREAM_SGI = 9` at `STREAM_SGI_PRIORITY = 24`.
- **`golden_vt_render` is not touched.** It is the before/after byte-identity gate for this
  change and its determinism rests on having exactly one executor plus a discrete-event
  driver loop. Its `streaming_fill_task` spawn at
  `src/bsp/rust/harness/golden_vt_render/src/main.rs:634` stays where it is.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `include/libdeluge/system.h` | Declares the new `deluge_in_audio_context()` boundary predicate | 1 |
| `crates/deluge_resource/src/sync.rs` | `Masked` gates on audio-context, not any-interrupt | 1 |
| `src/bsp/rust/src/services.rs` | Rust BSP providers (device: audio-handler depth flag; host: always false) | 1 |
| `src/bsp/rza1/system.c`, `src/bsp/host/host_bsp.c` | Legacy/C-host providers — alias to existing behaviour | 1 |
| `tests/32bit_unit_tests/mocks/mock_critical_section.cpp`, `tests/unit/mocks/hal_mocks.cpp` | Test-link providers | 1 |
| `crates/deluge_sample_reader/src/lib.rs`, `crates/deluge_sample_source/src/lib.rs` | Their own `#[cfg(test)]` stub providers | 1 |
| `src/bsp/rust/src/main.rs` (device block, ~lines 309-484) | `STREAM_EXEC` + SGI 9 wiring; fill task spawned there | 2 |
| `src/bsp/rust/harness/lens1_vt_sim/src/main.rs` | In-spin wedge attribution; fill task onto `HP_EXEC`; mutex-class selftest | 3, 4 |
| `src/bsp/rust/harness/lens1_vt_sim/src/preempt.rs` | In-spin flag exposed to `advance_to` | 3 |
| `src/bsp/rust/harness/lens1_vt_sim/src/selftest.rs` | The mutex-class selftest and its negative control | 4 |
| `src/bsp/rust/harness/lens1_vt_sim/sweep.sh` | Gates the two new selftest modes in `run_selftest` | 4 |
| `src/bsp/rust/src/main.rs` (host block, ~lines 1019-1075) | Third `"deluge-stream"` thread | 5 |
| The two specs above | Record the third-context finding and the scope decision | 6 |

---

## Task 1: Make the manager's critical section correct for a third preemptive context

This lands **first** and on its own. Tasks 2 and 5 are unsafe without it.

**Why (read this before touching anything).** `crates/deluge_resource/src/sync.rs` implements
an *asymmetric* critical section. Its module doc states the model: the fiber masks, and
"the audio ISR skips the mask entirely (gated on `!deluge_in_interrupt()`) because it can
never be preempted by the fiber, so its RMWs are already atomic w.r.t. it."

`deluge_in_interrupt()` on the Rust BSP device build is a **CPSR mode test**
(`src/bsp/rust/src/services.rs:105-113`): it returns true in *any* IRQ or FIQ context. So the
moment `streaming_fill_task` runs inside the SGI 9 handler, every
`m_get`/`m_set`/`m_rmw` it performs stops masking — while the audio SGI at priority 20
**preempts** SGI 9 and also skips masking. Two mutually-preempting contexts both skipping
the mask is exactly the unsynchronised read-modify-write the asymmetric design exists to
prevent, on the manager's `Cell<ChunkSlot>` state.

The fix encodes the actual invariant: only the **highest-priority** manager-touching context
may skip the mask. That context is audio, so the predicate must name audio, not "any
interrupt".

**Files:**
- Modify: `include/libdeluge/system.h` (add the declaration next to `deluge_in_interrupt`)
- Modify: `crates/deluge_resource/src/sync.rs:18-52` (extern block, `Masked::enter`, module doc)
- Modify: `crates/deluge_resource/src/sync.rs:95-169` (`stubs` module) — new stub + setter
- Modify: `crates/deluge_resource/src/sync.rs:171-214` (`tests` module) — new test
- Modify: `src/bsp/rust/src/services.rs:102-122` (device + host providers)
- Modify: `src/bsp/rza1/system.c:33` area, `src/bsp/host/host_bsp.c:88` area
- Modify: `tests/32bit_unit_tests/mocks/mock_critical_section.cpp:39` area
- Modify: `tests/unit/mocks/hal_mocks.cpp:10` area
- Modify: `crates/deluge_sample_reader/src/lib.rs:88` area, `crates/deluge_sample_source/src/lib.rs:79` area

**Interfaces:**
- Produces: `bool deluge_in_audio_context(void)` — C ABI. True iff the calling context is the
  audio render context (the one context that may skip the manager mask). Also produces the
  Rust-side device marker pair `crate::services::audio_context_enter() -> bool` and
  `crate::services::audio_context_exit(prev: bool)`, used by Task 2's sibling handler and by
  the existing audio handler.

- [ ] **Step 1: Write the failing test**

Add to `crates/deluge_resource/src/sync.rs`'s `tests` module. This asserts the new property:
a preemptible interrupt context (one that is *not* audio) must mask.

```rust
    #[test]
    fn a_non_audio_interrupt_context_still_masks() {
        // The streaming fill task runs in an interrupt context that the audio context
        // preempts, so it may NOT skip the mask — unlike audio itself. Regression guard for
        // the third-context hazard: gating on "am I in any interrupt" would silently make
        // this a no-op and race the audio path.
        cs_reset_counts();
        deluge_in_interrupt_set(true); // in an ISR ...
        deluge_in_audio_context_set(false); // ... but not the audio one
        let c = Cell::new(1u32);
        m_rmw(&c, |v| *v += 1);
        assert_eq!(c.get(), 2);
        assert_eq!(
            cs_enter_count(),
            1,
            "a non-audio interrupt context must mask: audio preempts it"
        );
        assert_eq!(cs_exit_count(), 1);
        deluge_in_interrupt_set(false); // restore for other tests on this thread
    }
```

Then retarget the existing `audio_skips_mask` test so it drives the new predicate rather
than the old one — replace its `deluge_in_interrupt_set(true);` line with both flags:

```rust
        deluge_in_audio_context_set(true); // audio ISR context
        deluge_in_interrupt_set(true);
```

and add `deluge_in_audio_context_set(false);` next to its existing restore line.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd crates && cargo test -p deluge_resource sync:: 2>&1 | tail -20`
Expected: FAIL to **compile** — `cannot find function 'deluge_in_audio_context_set' in this scope`.

- [ ] **Step 3: Add the stub provider and setter**

In `crates/deluge_resource/src/sync.rs`, inside `mod stubs`, add to the `thread_local!` block:

```rust
        static IN_AUDIO: Cell<bool> = const { Cell::new(false) };
```

and add the stub plus its setter alongside the existing `deluge_in_interrupt` pair:

```rust
    #[unsafe(no_mangle)]
    extern "C" fn deluge_in_audio_context() -> bool {
        IN_AUDIO.with(|f| f.get())
    }

    pub fn deluge_in_audio_context_set(v: bool) {
        IN_AUDIO.with(|f| f.set(v));
    }
```

- [ ] **Step 4: Change `Masked` to gate on the audio context**

In `crates/deluge_resource/src/sync.rs`, add to the extern block:

```rust
    fn deluge_in_audio_context() -> bool;
```

Replace `Masked::enter`'s predicate and its doc comment:

```rust
/// RAII asymmetric critical section. Enters iff called outside the audio render context
/// (`!deluge_in_audio_context()`) — a no-op on the audio path, which is already atomic
/// w.r.t. every context that can touch the manager because it preempts all of them.
///
/// The predicate is deliberately "am I the audio context", NOT "am I in an interrupt":
/// the streaming fill task runs in its own interrupt context that audio preempts, so it
/// must mask like any other preemptible caller. Gating on `deluge_in_interrupt()` would
/// make its critical sections silently vanish and race the audio path.
pub struct Masked {
    active: bool,
}

impl Masked {
    #[inline]
    pub fn enter() -> Self {
        // SAFETY: FFI to the BSP interrupt-mask primitives; no invariants beyond
        // ENTER/EXIT being balanced, which the Drop impl guarantees.
        let active = unsafe { !deluge_in_audio_context() };
        if active {
            unsafe { ENTER_CRITICAL_SECTION() };
        }
        Masked { active }
    }
}
```

Update the module doc's second paragraph (`sync.rs:1-11`) to describe three contexts:

```rust
//! B1 synchronization primitives for the Manager. Every access to the Manager's
//! shared `Cell` state routes through [`m_get`] / [`m_set`] / [`m_rmw`], which
//! wrap the access in an asymmetric critical section (`Masked`): every context masks the
//! minimal O(1) window EXCEPT the audio render context, which skips the mask entirely
//! (gated on `!deluge_in_audio_context()`) because it preempts every other
//! manager-touching context, so its RMWs are already atomic w.r.t. them.
//!
//! There are three such contexts on the Rust BSP: audio (highest, skips), the streaming
//! fill task (its own interrupt executor, masks), and thread mode / the worker fiber
//! (masks). The predicate is audio-specific rather than "in an interrupt" precisely so the
//! middle one keeps its mask — see [`Masked`].
//!
//! On host — where the preemptive TSan harness runs audio and streaming on real second and
//! third OS threads and `deluge_in_audio_context()` is always false — ALL threads mask, so
//! the global critical-section mutex serializes them and TSan sees no race.
```

- [ ] **Step 5: Run the crate tests to verify they pass**

Run: `cd crates && cargo test -p deluge_resource 2>&1 | tail -20`
Expected: PASS, including `a_non_audio_interrupt_context_still_masks` and the retargeted
`audio_skips_mask`.

- [ ] **Step 6: Declare the symbol at the C boundary**

In `include/libdeluge/system.h`, immediately after the existing `deluge_in_interrupt`
declaration, add:

```c
/// True iff the caller is executing in the audio render context — the one context that
/// preempts every other context touching shared audio/resource state, and therefore the one
/// context permitted to skip a mask that exists to exclude it. Distinct from
/// deluge_in_interrupt(): a lower-priority interrupt (the streaming fill executor) is "in an
/// interrupt" but is NOT the audio context, and must still mask.
bool deluge_in_audio_context(void);
```

- [ ] **Step 7: Provide it in the Rust BSP (device: real; host: always false)**

In `src/bsp/rust/src/services.rs`, immediately after the existing `deluge_in_interrupt`
device implementation, add the depth-tracked marker and the predicate. It is a save/restore
pair rather than a plain set/clear because the audio SGI can nest on top of the streaming
SGI, and when audio returns the context must revert to "streaming", not "thread mode":

```rust
/// Non-zero while the audio SGI handler is on the stack. Not an `AtomicBool`: audio can
/// nest on top of the lower-priority streaming SGI handler, so the marker must restore the
/// enclosing context on exit rather than clear unconditionally.
#[cfg(target_os = "none")]
static AUDIO_CONTEXT: AtomicU32 = AtomicU32::new(0);

/// Mark entry into the audio render context. Returns the previous marker state for
/// [`audio_context_exit`] to restore. Call ONLY from the audio SGI handler. [isr]
#[cfg(target_os = "none")]
pub fn audio_context_enter() -> bool {
    AUDIO_CONTEXT.fetch_add(1, Ordering::Relaxed) != 0
}

/// Undo one [`audio_context_enter`]. `_prev` is accepted (and ignored) so the call reads as
/// a balanced pair at every call site; the counter itself carries the nesting. [isr]
#[cfg(target_os = "none")]
pub fn audio_context_exit(_prev: bool) {
    AUDIO_CONTEXT.fetch_sub(1, Ordering::Relaxed);
}

/// True iff the audio SGI handler is on the stack. See `libdeluge/system.h`. [task] [isr]
#[cfg(target_os = "none")]
#[unsafe(no_mangle)]
pub extern "C" fn deluge_in_audio_context() -> bool {
    AUDIO_CONTEXT.load(Ordering::Relaxed) != 0
}

/// Host stand-in: host runs audio on a real OS thread with no interrupt context, and the
/// asymmetric skip is deliberately NOT taken there — all threads mask so the global
/// critical-section mutex serializes them (see `deluge_resource::sync`'s module doc).
/// [task] [isr]
#[cfg(not(target_os = "none"))]
#[unsafe(no_mangle)]
pub extern "C" fn deluge_in_audio_context() -> bool {
    false
}
```

Then mark the existing audio handler in `src/bsp/rust/src/main.rs` (currently
`audio_sgi_handler`, lines 333-337):

```rust
#[cfg(target_os = "none")]
fn audio_sgi_handler() {
    // Mark the audio context for `deluge_in_audio_context()` — the predicate the resource
    // manager's asymmetric critical section keys off. Must wrap the whole poll: any manager
    // access the audio task makes is inside it.
    let prev = crate::services::audio_context_enter();
    // SAFETY: only called from the SGI handler, after AUDIO_EXEC.start().
    unsafe { AUDIO_EXEC.on_interrupt() };
    crate::services::audio_context_exit(prev);
}
```

Confirm `AtomicU32` and `Ordering` are already imported in `services.rs` (they are —
`CS_DEPTH` at line 41 uses both). If the `#[cfg(target_os = "none")]` import is narrower than
the new use requires, widen the existing `use` rather than adding a second one.

- [ ] **Step 8: Provide it in the other five links**

Each of these keeps today's behaviour exactly: on those platforms the audio ISR is the only
preemptive context that touches the manager, so audio-context and in-interrupt coincide.

`src/bsp/rza1/system.c`, after `deluge_in_interrupt`:

```c
// On the legacy RZA1 BSP the audio render is the only interrupt context that reaches the
// resource manager, so "the audio context" and "in an interrupt" coincide here. Kept as an
// alias rather than a second mechanism: this BSP is committed for retirement, and the point
// of the separate predicate is a distinction only the Rust BSP draws.
bool deluge_in_audio_context(void) {
	return deluge_in_interrupt();
}
```

`src/bsp/host/host_bsp.c`, after `deluge_in_interrupt` — same body and a one-line comment
saying the C-host sim has no interrupt context at all, so this is always false exactly as
`deluge_in_interrupt` is.

`tests/32bit_unit_tests/mocks/mock_critical_section.cpp` and `tests/unit/mocks/hal_mocks.cpp`:
mirror whatever the neighbouring `deluge_in_interrupt` mock returns, with a comment saying it
mirrors it.

`crates/deluge_sample_reader/src/lib.rs` and `crates/deluge_sample_source/src/lib.rs`: add a
sibling stub next to each existing `deluge_in_interrupt` test stub:

```rust
    #[unsafe(no_mangle)]
    extern "C" fn deluge_in_audio_context() -> bool {
        false
    }
```

- [ ] **Step 9: Run the whole workspace, not just the crate you edited**

Adding an extern to `deluge_resource` obliges every link that pulls it in. Run all four:

```bash
cd crates && cargo test 2>&1 | tail -30
cd .. && ./dbt build Debug 2>&1 | tail -20
./dbt test 2>&1 | tail -20
cd src/bsp/rust && cargo device --release 2>&1 | tail -20
```

Expected: cargo workspace tests pass; `./dbt build Debug` 0 errors; `./dbt test` all specs
pass; `cargo device --release` produces the ARM ELF with no undefined
`deluge_in_audio_context`. A link error naming that symbol means a provider is missing from
step 8 — add it there, do not weaken the predicate.

- [ ] **Step 10: Commit**

```bash
git add -A include/libdeluge/system.h crates src/bsp tests
git commit -m "fix(resource): key the manager's mask skip on the audio context, not any interrupt

The asymmetric critical section let any interrupt context skip the mask, on the
reasoning that the audio ISR cannot be preempted by the fiber. That reasoning is
specific to audio, but the predicate was not: deluge_in_interrupt() is a CPSR
mode test, true in every IRQ. A second, lower-priority interrupt executor for
storage would therefore have had its critical sections silently vanish while the
audio SGI still preempted it and also skipped — an unsynchronised RMW on the
manager's Cell state, which is precisely what the asymmetry exists to prevent.

Introduce deluge_in_audio_context() and gate Masked on that instead. Audio still
skips; every other context, interrupt or not, masks. The five non-Rust-BSP links
alias it to their existing in-interrupt predicate, where the two genuinely
coincide."
```

---

## Task 2: The device `STREAM_EXEC`

**Files:**
- Modify: `src/bsp/rust/src/main.rs:309-341` (executor statics, SGI constants, handler)
- Modify: `src/bsp/rust/src/main.rs:412-484` (registration, start ordering, the spawn move)

**Interfaces:**
- Consumes: `crate::services::audio_context_enter`/`audio_context_exit` from Task 1 (used by
  the existing audio handler — the streaming handler deliberately does **not** mark itself,
  because it is not the audio context).
- Produces: nothing other tasks consume. Tasks 3-5 are independent of this one.

- [ ] **Step 1: Add the executor, its SGI, and its handler**

In `src/bsp/rust/src/main.rs`, immediately after the `AUDIO_SGI_PRIORITY` constant and the
`audio_sgi_handler` function, add the sibling set:

```rust
/// Preemptive Embassy executor for the storage streaming fill task. A second interrupt
/// executor at a priority *between* audio and thread mode, so audio still preempts the fill
/// and the fill preempts thread mode. That ordering is the whole point: once the worker
/// fiber's storage waits become non-yielding spins, a spin in thread mode must not be able
/// to monopolise the context that releases the filesystem mutex — which is this task, since
/// it holds that mutex across its SD await (`efatfs_fs::read_at` -> `with_fs`).
#[cfg(target_os = "none")]
static STREAM_EXEC: InterruptExecutor = InterruptExecutor::new();

/// GIC Software-Generated Interrupt id driving [`STREAM_EXEC`]. `AUDIO_SGI` is 8; 9 is the
/// next free one (SMP-free board, so SGIs are otherwise unused).
#[cfg(target_os = "none")]
const STREAM_SGI: u8 = 9;
/// Streaming SGI GIC priority. Numerically BELOW `AUDIO_SGI_PRIORITY` (20) so audio preempts
/// the fill, above thread mode so the fill preempts the fiber, and below PMR (31) so it is
/// forwarded at all. (Lower number = more urgent.)
#[cfg(target_os = "none")]
const STREAM_SGI_PRIORITY: u8 = 24;

/// GIC handler for [`STREAM_SGI`]: drive the streaming interrupt executor. Registered in the
/// HAL dispatch (`gic::register`), which already acks (GICC_IAR) before and EOIs (GICC_EOIR)
/// after, with IRQs re-enabled for nesting.
///
/// Deliberately does NOT mark the audio context (unlike [`audio_sgi_handler`]): this context
/// is preempted by audio, so its resource-manager accesses must keep masking. See
/// `deluge_resource::sync`'s module doc.
#[cfg(target_os = "none")]
fn stream_sgi_handler() {
    // SAFETY: only called from the SGI handler, after STREAM_EXEC.start().
    unsafe { STREAM_EXEC.on_interrupt() };
}
```

- [ ] **Step 2: Register and prioritise the SGI alongside audio**

Extend the existing `unsafe` block at `main.rs:416-419` so both SGIs are registered in the
same setup window:

```rust
    unsafe {
        rza1l_hal::gic::register(AUDIO_SGI as u16, audio_sgi_handler);
        rza1l_hal::gic::set_priority(AUDIO_SGI as u16, AUDIO_SGI_PRIORITY);
        rza1l_hal::gic::register(STREAM_SGI as u16, stream_sgi_handler);
        rza1l_hal::gic::set_priority(STREAM_SGI as u16, STREAM_SGI_PRIORITY);
    }
```

- [ ] **Step 3: Start it and spawn the fill task onto it**

`embassy_executor::set_gicd_base(0xE820_1000)` at `main.rs:434` is process-wide and already
precedes this — do not add a second call.

Replace the `set_audio_spawner` / `gic::enable` pair at `main.rs:440-441` with both executors
started in the same place, so neither GIC line is enabled before the executor it drives
exists:

```rust
    scheduler::set_audio_spawner(AUDIO_EXEC.start(AUDIO_SGI));
    unsafe { rza1l_hal::gic::enable(AUDIO_SGI as u16) };

    // Streaming fill executor. Spawned via a bootstrap task rather than directly: the fill
    // task's future is not `Send` (it holds a raw `DelugeResource*`), and
    // `InterruptExecutor::start` yields a `SendSpawner`. A bootstrap task IS `Send`, and once
    // it is running ON this executor it can obtain that executor's own local `Spawner` and
    // spawn the non-Send task without it ever crossing a context boundary.
    #[cfg(feature = "async_streaming_loader")]
    {
        let stream_spawner = STREAM_EXEC.start(STREAM_SGI);
        stream_spawner.spawn(stream_bootstrap().unwrap());
    }
    #[cfg(not(feature = "async_streaming_loader"))]
    let _ = STREAM_EXEC.start(STREAM_SGI);
    unsafe { rza1l_hal::gic::enable(STREAM_SGI as u16) };
```

Add the bootstrap task next to `app_task` (after `main`):

```rust
/// Bootstrap for [`STREAM_EXEC`]: obtain that executor's own `Spawner` from a task already
/// running on it, then spawn the (non-`Send`) streaming fill task locally. See the spawn site
/// in `main` for why the indirection is needed.
#[cfg(all(target_os = "none", feature = "async_streaming_loader"))]
#[embassy_executor::task]
async fn stream_bootstrap() {
    // SAFETY: this task only ever runs on STREAM_EXEC (it is spawned onto it and nowhere
    // else), so the Spawner yielded here belongs to STREAM_EXEC, and the non-Send task
    // spawned with it never leaves the executor it was created on.
    let spawner = unsafe { Spawner::for_current_executor() }.await;
    spawner.spawn(streaming_loader::streaming_fill_task().unwrap());
}
```

Then **delete** the old spawn from the thread executor's closure (`main.rs:482-483`) and
replace it with a pointer to where it went:

```rust
        // The async cluster-fill task is NOT spawned here any more: it runs on STREAM_EXEC,
        // its own preemptive interrupt executor (see `STREAM_EXEC` and `stream_bootstrap`).
        // A non-yielding storage spin in thread mode must not be able to starve it.
```

- [ ] **Step 4: Build for the device and confirm it links**

Run: `cd src/bsp/rust && cargo device --release 2>&1 | tail -30`
Expected: an ARM ELF, 0 errors.

If it fails with `the trait bound '...: Send' is not satisfied` pointing at
`stream_bootstrap`, the bootstrap future itself is not `Send`. Do not reach for
`unsafe impl Send` on `ProdOps` as a first move — first check what the compiler names as
non-Send in the bootstrap's own state; the only value it holds is a `Spawner`, and it holds
it *after* its single await point, so the state at suspension should be empty. If the
`for_current_executor()` future is itself non-`Send` in this embassy fork, the fallback is to
keep the bootstrap but obtain the spawner without awaiting across it, and only if that is
also impossible add `unsafe impl Send for ProdOps {}` in
`src/bsp/rust/src/streaming_loader.rs`'s `prod` module with a SAFETY comment recording that
the pointer is the process-wide singleton and the task is pinned to one executor. Record
whichever route you took in the commit message.

- [ ] **Step 5: Check the IRQ stack still fits**

Audio can now nest on top of the streaming handler, so peak IRQ-stack depth is a streaming
frame plus an audio frame where it used to be an audio frame alone. The IRQ stack is 8 KB
(`src/bsp/rust/linker/memory.x:18`, `IRQ_STACK_SIZE = 0x2000`).

Report the two frame sizes from the built ELF rather than guessing:

```bash
cd src/bsp/rust
arm-none-eabi-nm -S target/armv7a-deluge-eabihf/release/deluge-bsp-rust 2>/dev/null \
  | grep -iE 'irq_stack' || true
```

Then state in the commit message which functions occupy the nested path
(`stream_sgi_handler` -> `STREAM_EXEC.on_interrupt` -> `fill_once` -> `native_finish`, and
audio's own render on top). If you cannot obtain a stack-depth number without hardware, say
so plainly and flag it as an on-device check rather than asserting it fits. Do not silently
skip this step.

- [ ] **Step 6: Confirm nothing else regressed**

```bash
cd /home/kate/GitHub/DelugeFirmware
./dbt build Debug 2>&1 | tail -5
./dbt test 2>&1 | tail -5
```
Expected: 0 errors; all specs pass. (These build the host/C++ side, which this task does not
change — they are here to catch an accidental edit outside the `target_os = "none"` block.)

- [ ] **Step 7: Commit**

```bash
git add -A src/bsp/rust/src/main.rs
git commit -m "feat(bsp): run the streaming fill task on its own interrupt executor

The fill task held the filesystem mutex across its SD await while sitting on the
cooperative thread executor. Once the worker fiber's storage waits become
non-yielding spins, a spin in thread mode would wait for a mutex whose only
releaser sits on the executor the spin monopolises — a hang, not a slowdown.

Give the fill task a second InterruptExecutor on SGI 9 at GIC priority 24:
numerically below audio's 20 so audio still preempts the fill, above thread mode
so the fill preempts the fiber. The task is spawned through a Send bootstrap that
fetches the executor's own local Spawner, because the fill future holds a raw
manager pointer and InterruptExecutor::start only hands out a SendSpawner.

The new handler deliberately does not mark itself as the audio context: audio
preempts it, so its resource-manager accesses must keep masking."
```

---

## Task 3: Make a wedge inside a non-yielding spin identify itself

Small, independent, and it lands before Task 4 so Task 4's negative control produces a
legible failure instead of a misleading one.

**Why.** Today a spin that can never be satisfied does **not** report as a spin. Because
`preempt::progress_hook` returns `Progress::Advanced` on every clock advance, and
`sim_block::block_on` resets its stall counter on `Advanced`, the 10 000-stalled-poll budget
is unreachable in any real scenario (audio arms a timer every 2902 µs). The run instead dies
in `advance_to` with "virtual-time budget exhausted … scenario wedged" — which points at the
scenario, not at the spin that is actually stuck. R5a §3 records this as a thing to fix here.

**Files:**
- Modify: `src/bsp/rust/harness/lens1_vt_sim/src/preempt.rs` (an in-spin depth flag + accessor)
- Modify: `src/bsp/rust/harness/lens1_vt_sim/src/main.rs:366-386` (`advance_to`'s budget path)

**Interfaces:**
- Produces: `preempt::in_spin() -> bool` — true while a `sim_block::block_on` progress hook is
  on the stack. Task 4 does not depend on it, but its negative control reads better with it.

- [ ] **Step 1: Add the in-spin marker to `preempt`**

In `src/bsp/rust/harness/lens1_vt_sim/src/preempt.rs`, add next to `HOOK_INVOCATIONS`:

```rust
/// Non-zero while a [`progress_hook`] call is on the stack — i.e. while a non-yielding
/// `sim_block::block_on` spin is what is driving the clock. Read by `crate::advance_to` so a
/// budget exhaustion reached from inside a spin says so, instead of blaming the scenario.
/// A counter rather than a bool because the hook advances the clock and then pumps HP again,
/// and a future edit could nest.
static IN_SPIN: AtomicU64 = AtomicU64::new(0);

/// True while a non-yielding spin's progress hook is on the stack. See [`IN_SPIN`].
pub fn in_spin() -> bool {
    IN_SPIN.load(Ordering::Relaxed) != 0
}
```

Wrap the whole body of `progress_hook` so the marker covers the `advance_to` call it makes.
Rename the existing body to a private helper and have `progress_hook` bracket it:

```rust
pub fn progress_hook() -> Progress {
    IN_SPIN.fetch_add(1, Ordering::Relaxed);
    let r = progress_hook_inner();
    IN_SPIN.fetch_sub(1, Ordering::Relaxed);
    r
}

fn progress_hook_inner() -> Progress {
    // ... the existing body, unchanged ...
}
```

Keep the existing doc comment on `progress_hook` and add a line saying it marks
[`IN_SPIN`] for the duration.

- [ ] **Step 2: Write the failing assertion in `advance_to`**

In `src/bsp/rust/harness/lens1_vt_sim/src/main.rs`, replace the budget-exhaustion log in
`advance_to` (lines 371-377) with one that attributes the wedge:

```rust
    if next > budget_ticks {
        if crate::preempt::in_spin() {
            log::error!(
                "lens1-vt-sim: virtual-time budget ({budget_ticks}us) exhausted from INSIDE a \
                 non-yielding sim_block::block_on spin (next deadline at {next}us). The spin's \
                 future never became ready and the clock ran out chasing it — this is a wedged \
                 spin, not a slow scenario. Check what that future is waiting on and which \
                 executor can satisfy it."
            );
        } else {
            log::error!(
                "lens1-vt-sim: virtual-time budget ({budget_ticks}us) exhausted before the \
                 scenario completed (next deadline at {next}us) — scenario wedged"
            );
        }
        hard_exit(2);
    }
```

- [ ] **Step 3: Verify it compiles and the existing selftests still pass**

`./dbt harness run` takes no extra arguments — the registry entry for `lens1-vt-sim`
(`harness/registry.toml:77-86`) runs `./sweep.sh` with no arguments, which runs the selftests
as a precondition and then the ~25 minute sweep. For a fast selftest-only cycle use the
harness's own entry script with its `selftest` verb (this is the same script the registry
invokes — it is not running cargo by hand):

```bash
cd src/bsp/rust/harness/lens1_vt_sim && ./sweep.sh selftest 2>&1 | tail -20
```

Expected: `--selftest-block: PASS` and `--selftest-block-nested: PASS`. Both exercise the
hook, so the new marker is on the stack during them — that they still pass is what proves the
bracketing did not break the hook.

- [ ] **Step 4: Commit**

```bash
git add -A src/bsp/rust/harness/lens1_vt_sim
git commit -m "test(lens1): attribute a budget exhaustion reached from inside a spin

A spin that can never be satisfied did not report as a spin. progress_hook
returns Advanced on every clock advance and the block_on stall counter resets on
Advanced, so the stalled-poll budget is unreachable while audio keeps arming
timers. The run died in advance_to blaming the scenario for running out of
virtual time.

Mark the hook's own stack window and have advance_to distinguish the two cases,
so a wedged spin says it is a wedged spin."
```

---

## Task 4: Move the fill task onto `HP_EXEC` and prove the mutex-class spin

This is the test Phase 0 explicitly did not deliver: it proved the **timer**-class case (a
spin over a modeled SD read completes at exactly 525 µs) and left mutex-class spins
unproven, because on today's code a lock held by a MAIN-resident task can never be released
from inside a spin — the hook pumps `HP_EXEC` but cannot poll MAIN from within MAIN's own
`poll()`. Moving the fill task to `HP_EXEC` is what makes the mutex-class case satisfiable,
and this task proves it.

**Files:**
- Modify: `src/bsp/rust/harness/lens1_vt_sim/src/main.rs:560-582` (the spawn move)
- Modify: `src/bsp/rust/harness/lens1_vt_sim/src/selftest.rs` (the two new selftest bodies)
- Modify: `src/bsp/rust/harness/lens1_vt_sim/src/main.rs` (the two new selftest arms, and the
  `sim_latency::pump` spawn guard)
- Modify: `src/bsp/rust/harness/lens1_vt_sim/sweep.sh` (`run_selftest` gains the two modes;
  usage header updated)

**Interfaces:**
- Consumes: `preempt::hook_invocations() -> u64` and `preempt::pump_hp() -> bool` (existing);
  `preempt::in_spin() -> bool` from Task 3.
- Produces: `selftest::run_block_mutex(hp_spawner: embassy_executor::Spawner)` — the
  mutex-class selftest, invoked by `--selftest-block-mutex`; and
  `selftest::run_block_mutex_control(main_spawner: embassy_executor::Spawner, main_exec:
  &'static embassy_executor::raw::Executor)` — its negative control, invoked by
  `--selftest-block-mutex-control`.

- [ ] **Step 1: Write the failing selftest**

Add to `src/bsp/rust/harness/lens1_vt_sim/src/selftest.rs`. It builds the exact shape the
collapse will have: a MAIN-resident caller spins on a future that cannot complete until a
lock held by an `HP_EXEC`-resident task is released.

```rust
/// The mutex-class spin: a non-yielding `sim_block::block_on` on MAIN whose future needs a
/// lock currently held across an await by a task on `HP_EXEC`.
///
/// This is the case the timer-class selftest does NOT cover. There, the spin's future became
/// ready because the virtual clock moved; here it becomes ready only because *another task
/// ran to completion and dropped a guard*. Before the fill task moved to `HP_EXEC` this was
/// unsatisfiable by construction: the hook pumps `HP_EXEC` and cannot poll MAIN from inside
/// MAIN's own `poll()`, so a MAIN-held guard was never released.
///
/// Asserts three things, because any one alone can pass for the wrong reason:
///   1. the spin returns at all (it is not wedged);
///   2. `preempt::hook_invocations()` advanced across it — the completion came through the
///      hook mechanism under test, not because some enclosing loop happened to advance the
///      clock (the exact false pass `selftest.rs`'s module doc already warns about);
///   3. the guard really was contended — the holder task observed the spin waiting.
pub fn run_block_mutex(hp_spawner: embassy_executor::Spawner) {
    use core::sync::atomic::{AtomicBool, Ordering};
    use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
    use embassy_sync::mutex::Mutex;

    static LOCK: Mutex<CriticalSectionRawMutex, u32> = Mutex::new(0);
    static HOLDER_STARTED: AtomicBool = AtomicBool::new(false);
    static HOLDER_DONE: AtomicBool = AtomicBool::new(false);

    #[embassy_executor::task]
    async fn holder(
        started: &'static AtomicBool,
        done: &'static AtomicBool,
        lock: &'static Mutex<CriticalSectionRawMutex, u32>,
    ) {
        let mut g = lock.lock().await;
        started.store(true, Ordering::SeqCst);
        // Hold the guard ACROSS an await, exactly as `efatfs_fs::read_at` holds the FS mutex
        // across its SD read. Only the hook pumping HP_EXEC can get us past this.
        embassy_time::Timer::after_micros(500).await;
        *g += 1;
        done.store(true, Ordering::SeqCst);
    }

    hp_spawner.spawn(holder(&HOLDER_STARTED, &HOLDER_DONE, &LOCK).unwrap());
    // Let the holder actually take the lock before the spin starts, so the spin is genuinely
    // contended rather than trivially acquiring a free mutex. Same pre-pump rationale as the
    // nested timer-class selftest — see this module's doc.
    crate::preempt::pump_hp();
    assert!(
        HOLDER_STARTED.load(Ordering::SeqCst),
        "mutex selftest is vacuous: the HP_EXEC holder never took the lock, so the spin below \
         would acquire an uncontended mutex and prove nothing"
    );

    let hooks_before = crate::preempt::hook_invocations();
    let value = crate::sim_block::block_on(async {
        let g = LOCK.lock().await;
        *g
    });
    let hooks_after = crate::preempt::hook_invocations();

    assert!(
        HOLDER_DONE.load(Ordering::SeqCst),
        "the spin acquired the lock before the HP_EXEC holder finished with it — the guard was \
         not actually held across the await, so this proves nothing about mutex-class spins"
    );
    assert_eq!(
        value, 1,
        "expected the holder's increment to be visible once the spin acquired the lock"
    );
    assert!(
        hooks_after > hooks_before,
        "the spin completed WITHOUT the progress hook firing ({hooks_before} -> \
         {hooks_after}) — something other than the mechanism under test satisfied it"
    );
    println!("lens1-vt-sim: SELFTEST-BLOCK-MUTEX PASS (hooks {hooks_before} -> {hooks_after})");
}
```

Wire the mode in `src/bsp/rust/harness/lens1_vt_sim/src/main.rs`, next to where
`nested_selftest` is read (line ~556) and where the existing selftest branches are taken:

```rust
    let mutex_selftest = std::env::args().any(|a| a == "--selftest-block-mutex");
```

and, in the same place the other selftest branches run (after `BUDGET_TICKS`/watchdog are
published, so this mode is wedge-guarded like the others):

```rust
    if mutex_selftest {
        selftest::run_block_mutex(hp_spawner);
        return;
    }
```

- [ ] **Step 2: Run the positive selftest**

The new mode has to be wired into the harness's own runner or it will never be gated. In
`src/bsp/rust/harness/lens1_vt_sim/sweep.sh`, extend `run_selftest()` (around line 106) with a
third invocation following the exact shape of the two already there — same
`DELUGE_SD_IMAGE="$SELFTEST_IMAGE"`, same `timeout 60`, same PASS/FAIL log lines:

```bash
    local log_mutex="$LOG_DIR/selftest-block-mutex.log"
    if DELUGE_SD_IMAGE="$SELFTEST_IMAGE" timeout 60 "$BIN" --selftest-block-mutex \
        >"$log_mutex" 2>&1; then
        echo "  --selftest-block-mutex:  PASS" >&2
    else
        rc=$?
        echo "  --selftest-block-mutex:  FAIL (exit $rc) — see $log_mutex" >&2
        return 1
    fi
```

(Match the surrounding code's actual variable names for the log directory and binary — read
`run_selftest` before editing rather than assuming `$LOG_DIR`/`$BIN`.) Also update the usage
header at `sweep.sh:26-27`, which currently says the `selftest` verb runs
"`--selftest-block` + `--selftest-block-nested` only".

Then run:

```bash
cd src/bsp/rust/harness/lens1_vt_sim && ./sweep.sh selftest 2>&1 | tail -20
```

Expected: **PASS.** Be clear about why this is not a skipped red phase. The selftest spawns
its own holder onto `hp_spawner`, so it does not depend on the production spawn move in
Step 5 — it establishes the *mechanism* independently. The red/green pair that gives this
task its teeth is the positive test passing **and** the Step 3 control wedging; the control
is the half that can fail, and it is the half that would expose a vacuous positive.

What must not happen here is a wedge or a `hard_exit(2)`. Either means the progress hook
cannot drive an `HP_EXEC`-held guard at all, and both Task 5 and R5a Phase 2 rest on a false
premise. Stop and report rather than working around it.

- [ ] **Step 3: Add the negative control**

The control spawns the identical holder on **MAIN** and asserts the spin is *not* satisfiable
— the pre-move shape. Three mechanics make the difference between a real control and a
vacuous one, so follow them exactly:

1. **MAIN must be polled once before the spin**, or the holder never runs, the lock is free,
   and the spin trivially acquires it. The control therefore takes the raw MAIN executor and
   polls it, the same way `--selftest-block-nested` drives its own loop.
2. **Nothing else may be spawned in this mode** — in particular not `sim_latency::pump`. Any
   task that keeps arming timers makes `progress_hook` return `Advanced` forever, the stalled
   counter never accumulates, and the run dies in `advance_to` via `hard_exit(2)`, which is a
   raw `exit_group` syscall and therefore **not** catchable by `catch_unwind`. With no other
   timer source, `peek_next_deadline()` returns `None` right after the holder's single timer
   fires, the hook returns `Stalled`, and the wedge arrives as `sim_block`'s own assertion.
3. **A small spin budget**, so that assertion arrives in bounded time.

```rust
/// Negative control for [`run_block_mutex`]: the identical spin, with the holder on MAIN
/// instead of `HP_EXEC`. The hook pumps `HP_EXEC` and cannot poll MAIN from inside MAIN's own
/// `poll()`, so a MAIN-held guard is never released and the spin must NOT complete.
///
/// This is what makes the positive test meaningful: without it, `run_block_mutex` passing
/// could equally mean the lock was never contended.
///
/// `main_exec` is polled once here — not spawned-and-left — because the holder has to
/// actually take the lock before the spin starts. Nothing else may be spawned in this mode:
/// see the plan's Step 3 note on why a live timer source turns the expected wedge into an
/// uncatchable `hard_exit(2)`.
pub fn run_block_mutex_control(
    main_spawner: embassy_executor::Spawner,
    main_exec: &'static embassy_executor::raw::Executor,
) {
    use core::sync::atomic::{AtomicBool, Ordering};
    use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
    use embassy_sync::mutex::Mutex;

    static LOCK: Mutex<CriticalSectionRawMutex, u32> = Mutex::new(0);
    static STARTED: AtomicBool = AtomicBool::new(false);

    // A distinct task item from `run_block_mutex`'s `holder`: two `#[embassy_executor::task]`
    // functions each get their own pool, and sharing one across both modes would couple them.
    #[embassy_executor::task]
    async fn holder_on_main(
        started: &'static AtomicBool,
        lock: &'static Mutex<CriticalSectionRawMutex, u32>,
    ) {
        let mut g = lock.lock().await;
        started.store(true, Ordering::SeqCst);
        embassy_time::Timer::after_micros(500).await;
        *g += 1;
    }

    main_spawner.spawn(holder_on_main(&STARTED, &LOCK).unwrap());
    // SAFETY: single-threaded harness; this is the only poll of MAIN on the stack, and the
    // spin below reaches only `progress_hook`, which polls HP_EXEC — never MAIN.
    unsafe { main_exec.poll() };
    assert!(
        STARTED.load(Ordering::SeqCst),
        "control is vacuous: the MAIN holder never took the lock, so the spin below would \
         acquire a free mutex and prove nothing"
    );

    crate::sim_block::set_spin_budget(200);
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        crate::sim_block::block_on(async { *LOCK.lock().await })
    }));
    assert!(
        r.is_err(),
        "NEGATIVE CONTROL FAILED: a spin waiting on a MAIN-held guard completed. Either the \
         holder released it (check it really awaits while holding) or something is polling \
         MAIN from inside the spin — either way the positive mutex selftest proves nothing."
    );
    println!("lens1-vt-sim: SELFTEST-BLOCK-MUTEX-CONTROL PASS (spin correctly wedged)");
}
```

Wire `--selftest-block-mutex-control` alongside the other selftest flags. Like
`--selftest-block-nested`, it must suppress the whole app task set **and**
`hp_spawner.spawn(sd::sim_latency::pump())` — extend the existing `nested_selftest` guard on
that spawn to cover this mode too, e.g. a shared `let isolate_selftest = nested_selftest ||
mutex_control_selftest;` used for both. The `hp_spawner.spawn(sim_latency::pump())` line is
currently unconditional and its comment says so; update that comment to record the new
exception and why (a live timer source converts this control's expected wedge into an
uncatchable `hard_exit(2)`).

- [ ] **Step 4: Run the control to verify it detects**

Wire it into `run_selftest()` as a fourth invocation, exactly as Step 2 did for the positive
mode, then run:

```bash
cd src/bsp/rust/harness/lens1_vt_sim && ./sweep.sh selftest 2>&1 | tail -25
```

Expected: PASS, printing `SELFTEST-BLOCK-MUTEX-CONTROL PASS (spin correctly wedged)` — the
spin wedged and was caught.

Two failure shapes to distinguish rather than retry blindly:
- the assertion `NEGATIVE CONTROL FAILED` fires → the spin completed, so the positive test in
  Step 2 is vacuous. Fix the control's contention before continuing; do not weaken the
  assertion.
- the process exits 2 with a budget-exhaustion message → a timer source is still alive, so
  mechanic 2 above was not fully applied. Task 3's in-spin attribution should have made this
  message say "from INSIDE a non-yielding spin", which confirms the diagnosis quickly.

- [ ] **Step 5: Move the production fill task onto `HP_EXEC`**

In `src/bsp/rust/harness/lens1_vt_sim/src/main.rs`, inside the `if !nested_selftest {` block,
delete the `spawner.spawn(streaming_loader::streaming_fill_task()...)` line and its now-stale
comment (lines ~573-581, the comment beginning "On `spawner` (MAIN), the SAME executor"), and
spawn it on `hp_spawner` instead — placed right after the existing
`hp_spawner.spawn(sd::sim_latency::pump()...)`:

```rust
    // On HP_EXEC, not MAIN: this task holds the FS mutex across its SD await, so a
    // non-yielding `sim_block::block_on` in a MAIN-resident context must be able to drive it
    // to release. The hook pumps HP_EXEC and cannot poll MAIN from inside MAIN's own poll(),
    // so on MAIN this task was unreachable from a spin — the mutex-class case
    // `selftest::run_block_mutex` now covers, with `run_block_mutex_control` proving the MAIN
    // placement really was unsatisfiable. Mirrors the device's STREAM_EXEC.
    #[cfg(feature = "async_streaming_loader")]
    if !nested_selftest {
        hp_spawner.spawn(streaming_loader::streaming_fill_task().unwrap());
    }
```

Also update `preempt::init`'s doc comment (`preempt.rs:76-85`) and `pump_hp`'s reentrancy
doc (`preempt.rs:89-101`), both of which describe the move as still pending. `pump_hp`'s doc
predicts that this move introduces the reentrant-poll failure mode; state what is actually
true now: the fill task reaches storage through `efatfs_host_shim::read_at(...).await`, a
plain await, not `sim_block::block_on`, so it does not trigger the hook and the `IN_HP` guard
stays unreached — and the guard remains the thing that will catch it if that ever changes.

- [ ] **Step 6: Run all four selftests, then the full sweep**

```bash
cd src/bsp/rust/harness/lens1_vt_sim && ./sweep.sh selftest 2>&1 | tail -20
cd /home/kate/GitHub/DelugeFirmware && ./dbt harness run lens1-vt-sim 2>&1 | tail -30
```

The second command is the ~25 minute registry run (`./sweep.sh` with no verb): selftests as a
precondition, then the sweep.

Expected: all four selftests PASS, and the real run reaches a `LENS1_RESULT` line without
wedging. Lens 1's charter is wedge detection, not margin measurement — do **not** interpret
any counter delta as a performance result, and do not report one.

- [ ] **Step 7: Commit**

```bash
git add -A src/bsp/rust/harness/lens1_vt_sim
git commit -m "test(lens1): cover the mutex-class spin and run the fill task off MAIN

The host emulation proved only the timer half: a spin over a modeled SD read
completes because the clock moves. A spin waiting on a lock was unsatisfiable by
construction, because the hook pumps HP_EXEC and cannot poll MAIN from inside
MAIN's own poll() — and the fill task, the one thing that holds the FS mutex
across an await, sat on MAIN.

Move it to HP_EXEC, mirroring the device's streaming executor, and add the
selftest that was owed: a MAIN-resident spin on a guard held across an await by an
HP_EXEC task, asserting it completes, that the holder really was mid-await, and
that the progress hook is what satisfied it. Plus the negative control with the
holder back on MAIN, which must wedge — without it, the positive test could pass
on an uncontended lock."
```

---

## Task 5: A third executor thread for the threaded host BSP

**Files:**
- Modify: `src/bsp/rust/src/main.rs:1019-1075` (the host block: new thread, spawn moved)

**Interfaces:**
- Consumes: `crate::services::deluge_in_audio_context` from Task 1 (indirectly — it is what
  makes all three host threads mask and therefore mutually exclude).
- Produces: nothing other tasks consume.

**Why this and not just device.** The host BSP already runs audio on a real second OS thread
(`"deluge-audio"`), and the fill task shares the `"deluge-bsp-host-app"` thread with the
worker-fiber pump loop and the C++ enqueue path. That is the same starvation shape the device
had: a non-yielding spin on the app thread would keep the fill task from ever running. It is
also the model `preemptive-race-tsan` exercises, so a third thread is how the new concurrency
gets a race gate at all.

- [ ] **Step 1: Add the streaming thread**

In `src/bsp/rust/src/main.rs`, after the `"deluge-audio"` thread block and its
`audio_spawner_ready.wait()`, add a third thread. It follows the audio thread's barrier
pattern exactly, because the fill task must exist before `deluge_app_init` can enqueue into
the loader queue:

```rust
        // --- Third host executor thread for the streaming fill task ------------
        // Device runs the fill task on `STREAM_EXEC`, a preemptive interrupt executor at a
        // priority between audio and thread mode. Host has no interrupt context, so a
        // dedicated `std::thread` with its own executor is the analogue — real OS-thread
        // preemption instead of an SGI, with the same property that matters: a non-yielding
        // storage spin on the app thread cannot starve the fill task.
        //
        // Mutual exclusion comes for free: `deluge_in_audio_context()` is always false on
        // host, so every thread masks and the global critical-section mutex serializes all
        // three (see `deluge_resource::sync`'s module doc).
        //
        // Same barrier rationale as the audio thread above: the loader queue must have an
        // owner before `host_app_task` reaches `deluge_app_init`, or early enqueues sit
        // undrained until the first later `FILL_WAKE`.
        #[cfg(feature = "async_streaming_loader")]
        let stream_ready = Arc::new(Barrier::new(2));
        #[cfg(feature = "async_streaming_loader")]
        {
            let stream_barrier = Arc::clone(&stream_ready);
            std::thread::Builder::new()
                .name("deluge-stream".into())
                .spawn(move || {
                    let executor: &'static mut Executor = Box::leak(Box::new(Executor::new()));
                    executor.run(|spawner: Spawner| {
                        spawner.spawn(streaming_loader::streaming_fill_task().unwrap());
                        log::info!(
                            "deluge-bsp-rust: host streaming executor up on thread {:?}",
                            std::thread::current().name()
                        );
                        stream_barrier.wait();
                    });
                })
                .expect("spawning the host streaming executor thread");
            stream_ready.wait();
        }
```

- [ ] **Step 2: Remove the fill task from the app thread**

Delete the `#[cfg(feature = "async_streaming_loader")]
spawner.spawn(streaming_loader::streaming_fill_task().unwrap());` pair and its comment from
the `"deluge-bsp-host-app"` closure (`main.rs:1035-1042`), leaving a pointer:

```rust
                    // The async cluster-fill task is NOT spawned here any more: it owns the
                    // `"deluge-stream"` thread above, mirroring the device's STREAM_EXEC. A
                    // non-yielding storage spin on THIS thread must not be able to starve it.
```

- [ ] **Step 3: Build the host BSP and run its own test suite**

```bash
cd src/bsp/rust
cargo test 2>&1 | tail -30
```
Expected: the BSP host suite passes, `streaming_fill_host` included. If `streaming_fill_host`
now hangs, the barrier is in the wrong place relative to `deluge_app_init` — fix the ordering,
do not remove the barrier.

- [ ] **Step 4: Run the race gate**

Run: `./dbt harness run preemptive-race-tsan 2>&1 | tail -30`
Expected: `open_findings=0`. A new finding here is the whole reason this task exists — report
it with the TSan stack rather than suppressing it, and treat a new race as a blocking result.

- [ ] **Step 5: Run the golden differential**

```bash
cd /home/kate/GitHub/DelugeFirmware
./dbt harness run golden-embassy-diff 2>&1 | tail -30
```

`golden_vt_render` has its own `main` and its own single executor, so this task does not move
its fill task — but Task 1 changed `Masked` for every link, so this gate must run. Expected:
byte-identical on cordae, and on icoustic against its 2026-08-08 re-baseline. `highsiderr`
flips on any codegen change (a known uninitialised-stack-read suspect) — gate on cordae and
icoustic, and if highsiderr diverges, say so and identify it as the known flip rather than
presenting it as a regression. Never pass `update`.

- [ ] **Step 6: Commit**

```bash
git add -A src/bsp/rust/src/main.rs
git commit -m "feat(bsp): give the host streaming fill task its own executor thread

The host BSP ran the fill task on the same thread as the worker-fiber pump loop
and the C++ enqueue path, so a non-yielding storage spin there would starve the
one task that releases the FS mutex — the same shape the device just fixed with
its streaming interrupt executor.

Give it a third OS thread with its own executor, barrier-synchronised before
deluge_app_init the way the audio thread already is, so the loader queue has an
owner before the first enqueue. Mutual exclusion needs nothing new: audio-context
is always false on host, so all three threads mask and the global
critical-section mutex serializes them."
```

---

## Task 6: Record what this rung actually found

**Files:**
- Modify: `docs/superpowers/specs/2026-08-07-r5a-fiber-io-retirement-design.md` §4 Phase 1
- Modify: `docs/superpowers/specs/2026-08-08-storage-execution-model-end-state-design.md` §5
  (the Phase 1 ladder row) and §8 (Known-uncovered)

- [ ] **Step 1: Amend the R5a spec's Phase 1 section**

Replace §4's Phase 1 bullet list with one that reflects what was actually required. The spec
currently says only "mirrors the existing `AUDIO_EXEC` wiring", which understates it. Add,
in the spec's own voice:

```markdown
### Phase 1 — `STREAM_EXEC` (the prerequisite)

**A third preemptive context breaks the manager's asymmetric critical section, and that has
to be fixed first.** `deluge_resource::sync::Masked` skipped its mask whenever
`deluge_in_interrupt()` was true, on the reasoning that the audio ISR cannot be preempted by
the fiber. The reasoning is audio-specific; the predicate was not — on the Rust BSP it is a
CPSR mode test, true in every IRQ. Promoting the fill task into an SGI handler would
therefore have deleted its critical sections while the audio SGI still preempted it and also
skipped, racing the manager's `Cell` state. Fixed by introducing
`deluge_in_audio_context()` (true only inside the audio SGI handler, nesting-safe) and
gating `Masked` on that; the legacy RZA1 and C-host links alias it to their existing
in-interrupt predicate, where the two genuinely coincide.

- **Device:** a second `InterruptExecutor` for `streaming_fill_task` on SGI 9 at GIC
  priority 24 — numerically below `AUDIO_SGI_PRIORITY` (20) so audio still preempts the
  fill, above thread mode so the fill preempts the fiber. Spawned through a `Send` bootstrap
  task that fetches the executor's own local `Spawner`, because the fill future holds a raw
  `DelugeResource*` and `InterruptExecutor::start` only yields a `SendSpawner`.
- **Lens 1:** the fill task moves from MAIN to `HP_EXEC`, the executor the in-spin progress
  hook already pumps.
- **Threaded host BSP:** a third `"deluge-stream"` OS thread with its own executor,
  barrier-synchronised before `deluge_app_init` like the audio thread. This is also what
  gives the new concurrency a race gate, since `preemptive-race-tsan` runs this model.
- **`golden_vt_render` is deliberately unchanged** — one executor plus a discrete-event
  driver loop is its whole determinism story, and it is the before/after byte-identity gate
  for this very change.
- **The mutex-class test Phase 0 owed** is delivered as `--selftest-block-mutex`: a
  MAIN-resident `sim_block::block_on` on a guard held across an await by an `HP_EXEC` task,
  asserting completion, real contention, and that the progress hook is what satisfied it —
  with `--selftest-block-mutex-control` (holder back on MAIN) required to wedge, so the
  positive test cannot pass on an uncontended lock.
- **Self-identifying wedge diagnosis:** `preempt::in_spin()` marks the hook's stack window
  and `advance_to` distinguishes "budget exhausted from inside a spin" from "scenario ran
  long", so the failure mode §3 describes no longer misattributes itself.
- `Mutex::lock().await` from the promoted task stays deadlock-free: awaiting parks *that*
  executor rather than spinning, so thread mode can still progress.
```

- [ ] **Step 2: Update the end-state ladder row**

In `docs/superpowers/specs/2026-08-08-storage-execution-model-end-state-design.md` §5, the
Phase 1 row's Content cell ends "**Must carry the mutex-class test Phase 0 explicitly did
not prove** … and the self-identifying wedge diagnosis from R5a §3." Replace those two
clauses with a statement that both are now delivered, naming
`--selftest-block-mutex` / `--selftest-block-mutex-control` and `preempt::in_spin()`, and add
the audio-context prerequisite in one clause.

In §8 (Known-uncovered), the bullet "**Mutex-class spins are unproven on host.** Phase 0
proved the Timer-class case only … Phase 1 must carry the test." — replace it with a line
recording that Phase 1 carried it, naming the two selftest modes, and keep §8 honest about
what is still uncovered (MAIN starvation during a nested spin remains uncovered; the
`golden_vt_render/build.rs` unfiltered `collect_objs` is still owed before Phase 2; on-device
debt now additionally includes the SGI-9 priority ordering and the nested IRQ-stack depth).

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs
git commit -m "docs(storage): record the third-context finding and what the rung delivered

The design said the streaming executor 'mirrors the existing AUDIO_EXEC wiring'.
It does not: a third preemptive context breaks the resource manager's asymmetric
critical section, because its mask-skip predicate asked 'am I in an interrupt'
when the invariant it encodes is 'am I the context that preempts all the others'.
Write that down where the next reader will look, along with the four-context
scope decision, the mutex-class selftest and its negative control, and the
in-spin wedge attribution."
```

---

## Gates for the whole rung

Run these together at the end, and report each result as observed rather than as expected:

| Gate | Command | What it proves |
|---|---|---|
| Device link | `cd src/bsp/rust && cargo device --release` | SGI 9 wiring and the bootstrap spawn compile and link for ARM |
| Firmware C++ | `./dbt build Debug` | the new C-ABI symbol resolves in the app build |
| Unit specs | `./dbt test` | no CppSpec regression from the mask change |
| Rust workspace | `cd crates && cargo test` | the manager's masking contract, including the new third-context test |
| BSP host suite | `cd src/bsp/rust && cargo test` | the third thread boots and `streaming_fill_host` still passes |
| Lens 1 wedge detection | `./dbt harness run lens1-vt-sim` (runs the four selftests as a precondition, then the sweep) | no wedge; the mutex-class case is satisfiable and its control still detects |
| Lens 2 races | `./dbt harness run preemptive-race-tsan` | the new executor/thread introduces no race (`open_findings=0`) |
| Golden differential | `./dbt harness run golden-embassy-diff` | rendered output unchanged through the mask and executor changes (cordae + icoustic; highsiderr flips on any codegen) |
| `fs_differential` | `./dbt harness run fs-differential` | FS correctness unaffected |

**Hardware is the one gate this rung cannot self-serve.** SGI priority ordering is a hardware
property, and so is the nested IRQ-stack depth (audio frame on top of a streaming frame,
8 KB total). Both are on-device checks. Report them as owed; do not describe the rung as
verified without them.

---

## Self-review notes

- **Spec coverage.** R5a §4 Phase 1 has four bullets: device executor (Task 2), host
  emulation (Task 4 — Phase 0a already built the emulation; Phase 1's part is putting the
  fill task where the hook can reach it), the durable regression test replacing 0d's spike
  (Task 4), and the `Mutex::lock().await` deadlock-freedom claim (recorded in Task 6's spec
  text; it is an argument, not code). The end-state ladder adds two requirements the R5a spec
  does not: the mutex-class test (Task 4) and the self-identifying wedge diagnosis (Task 3).
  Task 1 covers no spec requirement — it is a hazard this plan found, which is why Task 6
  writes it back into the spec.
- **Deliberately out of scope.** Collapsing any `block_on_fiber` site (Phase 2), deleting
  `SD_BUS` / `off_fiber_instant` / `RESOURCE_SD` (Phase 3), and the
  `golden_vt_render/build.rs` unfiltered `collect_objs` fix (owed before Phase 2, not needed
  by this rung since no `.cpp` files move here).
- **Known-risky steps, flagged rather than assumed away.** Task 2 Step 4 (the `Send`
  bootstrap may not compile as written — fallback documented, with the reason to prefer it),
  Task 2 Step 5 (nested IRQ-stack depth may not be answerable off-hardware), Task 4 Step 2
  (the positive selftest may pass immediately, which is a legitimate outcome and is stated as
  such), and Task 5 Step 4 (a new TSan finding is a blocking result, not something to
  suppress).
