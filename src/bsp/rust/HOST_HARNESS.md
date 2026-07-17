# Host harness (`deluge-bsp-rust`, M1)

The Rust/Embassy BSP (`src/bsp/rust`) dual-targets: `cargo device`
(`armv7a-none-eabihf`) builds the real Deluge firmware image; plain `cargo
build`/`cargo run`/`cargo test` (no `--target`, or `--target
x86_64-unknown-linux-gnu`) build a **host** binary that runs the same
`scheduler.rs`/`fiber.rs`/`sd.rs`/`services.rs` modules on `std`, off
hardware. All host-only code is `#[cfg(not(target_os = "none"))]`; the device
build/behaviour is unchanged by any of this (see `cargo device` in the repo
root's normal CI — still links the ARM firmware image byte-for-byte the same
as before M1).

## What M1 runs on host

1. **Fiber selftest** (`fiber::selftest()`, Task 3) — exercises the
   cooperative context switch in isolation (main → fiber → yield → resume →
   complete) on the host `corosensei`-backed switch. Run by `deluge-rust`'s
   host `fn main()` at startup.
2. **SD round-trip** (`sd.rs`'s `host_disk` shim, Task 4) — writes a
   deterministic pattern to a file-backed block device via the real
   `deluge_block_write`/`deluge_block_read` C-ABI, reads it back, and asserts
   the bytes survive.
3. **Scheduler/fiber concurrency exercise** (`tests/scheduler_host.rs` +
   `tests/support/scheduler_host_exercise.rs`, Task 5 — this document) — brings
   up a real host `embassy_executor::Executor` on its own thread (mirroring
   `main.rs`'s device `executor.run(...)` and the deluge-sdk `host::run`
   template) and drives `scheduler.rs` through the actual `scheduler_api.h`
   C ABI: two repeating tasks, an once-task, a conditional task (predicate
   gated), a block/unblock cycle, a `runTask` nudge, and a `deluge_worker_run`
   fiber op that suspends in `scheduler::yield()` (`fiber::yield_until`) until
   its own gate opens — exercising the corosensei stack switch under the
   executor, not just the queue plumbing around it. All driving/assertion
   calls after registration run on a *different* OS thread than the executor,
   so this is genuine cross-thread concurrency into the scheduler's C ABI, not
   just async interleaving on one thread.

`cargo build`/`cargo run` (host) still only run 1–2 at startup (unchanged from
Task 4); the Task 5 exercise lives in `tests/` + `examples/` and is invoked
separately (`cargo test`, or the TSan invocation below) — it does not run as
part of the plain host binary's `fn main()`.

### Why `tests/` needs `#[path]`, and why there's also an `examples/` entry point

`deluge-bsp-rust` is bin-only (`[[bin]] name = "deluge-rust"`, no `[lib]`
target — see `Cargo.toml`), so nothing under `tests/`/`examples/` can `use
deluge_bsp_rust::...`. Both entry points instead declare the exact same `mod
fiber; mod scheduler;` tree `main.rs` does, via `#[path]` pointing at the real
sources (`src/fiber.rs`, `src/scheduler.rs`) — this **recompiles those two
files unmodified** into the test/example binary rather than duplicating their
logic; every symbol used (the `scheduler_api.h` C-ABI fns, plus
`fiber::deluge_worker_run`/`worker_poll`/`WORKER_WAKE`) was already `pub`
before Task 5, so no visibility change to `scheduler.rs`/`fiber.rs` was
needed. The actual task bodies + assertions live once, in
`tests/support/scheduler_host_exercise.rs::run()` (a `tests/<subdir>/` file,
so Cargo does not treat it as its own test binary), shared by:

- `tests/scheduler_host.rs` — the normal `cargo test` entry point (plain,
  unsanitized).
- `examples/scheduler_host_tsan.rs` — a plain `fn main()` binary for the TSan
  run (see below for why `cargo test`'s `--test` harness can't be used there).

## TSan invocation

### Finding: `--features sanitize` cannot link under `-Zsanitizer=thread`

The crate's `sanitize` feature (`corosensei/sanitizer`) was believed to be the
prerequisite for a clean TSan run over the corosensei fiber switch (see the
feature's original doc comment). Task 5 found this is wrong: `corosensei`'s
`sanitizer` feature wires `__sanitizer_start_switch_fiber`/
`__sanitizer_finish_switch_fiber`/`__asan_unpoison_memory_region` — **AddressSanitizer's**
fiber-switch annotation API (`corosensei::sanitizer`'s own module doc literally
says "Runtime support for address sanitizer"). ThreadSanitizer's runtime
(`librustc-nightly_rt.tsan.a`) does not implement that API at all — `nm` on it
shows a completely different, TSan-specific fiber family instead
(`__tsan_create_fiber`, `__tsan_destroy_fiber`, `__tsan_switch_to_fiber`,
`__tsan_get_current_fiber`, `__tsan_set_fiber_name`). Building
`--features sanitize` under `-Zsanitizer=thread` fails to link with three
undefined symbols (`__sanitizer_start_switch_fiber`,
`__sanitizer_finish_switch_fiber`, `__asan_unpoison_memory_region`), confirmed
experimentally. **The M1 Task 5 TSan run does not enable `--features
sanitize`** — and doesn't need to: TSan's happens-before tracking is per real
OS thread, not per stack region, so a same-thread corosensei coroutine switch
(no new pthread involved) has no other thread to misattribute state to,
unlike ASan's per-stack redzone/fake-frame tracking which does need the
annotation. `Cargo.toml`'s `sanitize` feature comment has been corrected to
record this; the feature itself is untouched (still gates
`corosensei/sanitizer`, still useful for a future ASan pass under
`-Zsanitizer=address`).

### Finding: plain `-Zsanitizer=thread` (no `-Zbuild-std`) gives an unusable verdict

A first attempt ran `RUSTFLAGS="-Zsanitizer=thread
-Cunsafe-allow-abi-mismatch=sanitizer" cargo +nightly test --target
x86_64-unknown-linux-gnu scheduler_host` (uninstrumented, prebuilt sysroot
`std`). It ran, and reported a "data race" inside `embassy_time::driver_std`'s
lazy `Inner::init()` (a `std::sync::Mutex`-guarded one-time init racing its
own spawned `alarm_thread`). This is almost certainly a **false positive**,
not a real bug: `critical-section`'s host backend (which every
`embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex` in
`scheduler.rs` — `Signal`, `Mutex` — ultimately routes through) and
`embassy-time`'s own driver both serialize via `std::sync::Mutex`, and modern
Rust's `std::sync::Mutex` on Linux is a futex-based, non-pthread
implementation. Compiled *without instrumentation* (the default, prebuilt
sysroot `std`), its internal atomic lock/unlock operations are invisible to
TSan's shadow-memory happens-before tracker — so a genuinely-correctly-locked
critical section can look like an unguarded race. This is a well-known
Rust+TSan sharp edge, which is why the official guidance is to also rebuild
`std` with the sanitizer (`-Zbuild-std`). Given scheduler.rs's entire
cross-thread synchronization story (task-slot signalling, the
add/block/unblock C-ABI) rests on exactly these primitives, a run without
`-Zbuild-std` cannot give a trustworthy verdict about `scheduler.rs` itself —
so this result was discarded and `-Zbuild-std` was pursued instead of settling
for it.

### Finding: `-Zbuild-std` + host-triple == target-triple hits a Cargo bug — worked around with a custom target spec

Plain `-Zbuild-std` (`cargo +nightly test -Zbuild-std --target
x86_64-unknown-linux-gnu ...`), and also the `-Zhost-config
-Ztarget-applies-to-host` variant that explicitly separates host- and
target-context `rustflags`, both hit a reproducible `error[E0152]: duplicate
lang item in crate core: sized` — the unit graph ends up with two different
`core` artifacts for one crate, whenever the host and target triple strings
are identical (the deluge-bsp-rust crate needs `bindgen` as a build-dependency
even though its `build.rs` early-returns for host builds — Cargo still
compiles declared build-dependencies regardless — and that build-dependency
graph overlaps target-context crates like `critical-section`/`memchr`/
`byteorder`, apparently confusing `-Zbuild-std`'s host/target unit
deduplication on nightly `1.98.0`). **Workaround** (standard technique for
self-hosting sanitizer builds on the same triple): a **custom target-spec
JSON**, byte-identical to `x86_64-unknown-linux-gnu` except its filename/
target name, so Cargo's unit graph can no longer alias "host" and "target" —
`sanitizer/x86_64-unknown-linux-gnu-tsan.json` in this directory (generated via
`rustc +nightly -Z unstable-options --print target-spec-json --target
x86_64-unknown-linux-gnu`, then renamed). With this, `-Zbuild-std` compiles
cleanly through the whole dependency graph.

### Finding: `cargo test`'s `--test` harness re-triggers the same bug (needs `libtest`) — worked around with a plain example binary

Once the custom target made `cargo build`/`cargo run --example` work, `cargo
test --test scheduler_host` under the same flags hit the **same** E0152 error
again — `-Zbuild-std=...,test` (needed for `libtest`) reintroduces a
host/target unit collision. Rather than fight this further, the exercise body
was factored out (see above) so it can run as a plain `fn main()` — `cargo
run --example scheduler_host_tsan` needs no `libtest` at all and sidesteps the
bug entirely, while `tests/scheduler_host.rs` remains the normal (unsanitized)
`cargo test` entry point, unaffected.

### One-time environment setup

The custom target has no prebuilt sysroot, so rustc emits a linker reference
to `<sysroot>/lib/rustlib/x86_64-unknown-linux-gnu-tsan/lib/librustc-nightly_rt.tsan.a`
that doesn't exist. Point it at the real one (one-time, outside the repo, in
the Rust toolchain's own directory — not a repo file):

```sh
SYSROOT=$(rustc +nightly --print sysroot)
mkdir -p "$SYSROOT/lib/rustlib/x86_64-unknown-linux-gnu-tsan/lib"
ln -sf "$SYSROOT/lib/rustlib/x86_64-unknown-linux-gnu/lib/librustc-nightly_rt.tsan.a" \
       "$SYSROOT/lib/rustlib/x86_64-unknown-linux-gnu-tsan/lib/librustc-nightly_rt.tsan.a"
```

### The exact invocation (from `src/bsp/rust`)

```sh
RUSTFLAGS="-Zsanitizer=thread" TSAN_OPTIONS="halt_on_error=1" \
  cargo +nightly run \
  -Zbuild-std=core,alloc,std,panic_abort \
  -Zjson-target-spec \
  --target sanitizer/x86_64-unknown-linux-gnu-tsan.json \
  --example scheduler_host_tsan
```

(`nightly` is this directory's default toolchain already, via
`rust-toolchain.toml`; `+nightly` is explicit here for clarity.
`rust-src`/`llvm-tools-preview` components, required for `-Zbuild-std`, are
already listed in `rust-toolchain.toml`.)

### Proof TSan is live: statically-linked interceptors + a fired negative control

The resulting binary genuinely links the TSan runtime (statically —
`nm | grep -c __tsan_` reports 50 symbols including `__tsan_init`,
`__tsan_read4`, `__tsan_write4`; `file` shows it's still a normal dynamically-linked
ELF, i.e. the TSan runtime itself is statically archived in, which is the
standard `librustc-nightly_rt.tsan.a` link shape).

A throwaway negative control (`examples/tsan_negative_control.rs`, NOT
committed — deleted immediately after use) span 4 threads each doing
`for _ in 0..100_000 { *COUNTER += 1 }` on a shared `static mut i32` with no
synchronization, built and run with the *exact* invocation above (custom
target, `-Zbuild-std`, same flags). It reliably fired:

```
==================
WARNING: ThreadSanitizer: data race (pid=580944)
  Write of size 4 at 0x55f709715050 by thread T3:
    #0 tsan_negative_control::main::{closure#0} .../examples/tsan_negative_control.rs:16:21
    ...
  Previous write of size 4 at 0x55f709715050 by thread T1:
    #0 tsan_negative_control::main::{closure#0} .../examples/tsan_negative_control.rs:16:21
    ...
SUMMARY: ThreadSanitizer: data race ... in tsan_negative_control::main::{closure#0}
==================
```
exit code 66 (TSan's `halt_on_error` convention). This confirms TSan is
genuinely instrumenting and detecting races under this invocation — a clean
result on the real exercise means "no race found," not "TSan didn't run."

## Verdict: TSan CLEAN

`scheduler_host_tsan` (the real exercise: two repeating tasks, a once-task, a
predicate-gated conditional task, a block/unblock cycle driven from a second
OS thread, a `runTask` nudge, and a `deluge_worker_run` fiber op that suspends
in `yield_until` until its own gate opens) ran **14 consecutive times** under
the invocation above — every run printed `scheduler_host_tsan: PASSED` (all
progress assertions held within the 15 s wall-clock deadlock watchdog) and
exited 0, with **zero** TSan `WARNING` output of any kind across all 14 runs.

This certifies: for the interleavings actually exercised (repeating-task
progress, cross-thread block/unblock/runTask into the scheduler's C-ABI,
predicate-gated conditional dispatch, and the corosensei fiber
suspend/resume/completion cycle under the executor), `scheduler.rs`'s and
`fiber.rs`'s synchronization is race-free — under a properly `-Zbuild-std`-
instrumented `std`, so `critical-section`'s/`embassy-time`'s internal
`std::sync::Mutex` usage is visible to the detector (not the blind spot that
made the earlier plain run's report untrustworthy). As with any TSan run,
this is one (large, seeded) concurrent schedule, not an exhaustive proof; the
progress-assertion battery + the repeated-run count give the same style of
confidence as `tests/fatfs_stress`'s TSan verdict (see `RESULTS.md` there).

## Scope notes

- Only the host `platform-std` executor thread + the driving/test thread are
  exercised. A second `std::thread` executor modelling the audio spawner
  (`SendSpawner`/`InterruptExecutor` path) is explicitly optional per the M1
  scoping doc and was not added — `AUDIO_EXEC`/`set_audio_spawner`'s
  cross-context handoff remains device-only-verified.
- No C++ app is linked on host in M1 (see `build.rs`); the synthetic
  `extern "C" fn` task bodies here stand in for the real registered Deluge
  tasks, matching the same C-ABI shape (`TaskHandle = extern "C" fn()`,
  `RunCondition = Option<unsafe extern "C" fn() -> bool>`) the C++ app would
  hand the scheduler.
- `RES_ERROR`/`RES_WRPRT`/`STA_NODISK`/`STA_PROTECT` dead-code warnings in
  `sd.rs`, and `set_spawner`/`set_audio_spawner` "never used" warnings in a
  plain (non-test/example) `cargo build`, are pre-existing (Task 4) and
  unrelated to Task 5 — the test/example targets themselves use both
  `set_spawner` and (transitively, via `scheduler.rs`) `set_audio_spawner`'s
  sibling code path, so those specific warnings disappear when building the
  test/example targets.

## M2–M5 path (from the scoping doc)

- **M2** — host peripheral stubs (`pic`/`oled`/`cv_gate`), a local shim
  standing in for the real hardware protocol.
- **M3** — host C++ app build: a host-ABI bindgen target (today's `mod sys` is
  device-only; M1's `fiber.rs`/`scheduler.rs`/`sd.rs` host stand-ins for
  `RunCondition`/`DelugeStatus`/etc. would fold into it), link the real
  `deluge_app` static lib against the host binary, host-vs-device ABI
  round-trip as the key spike.
- **M4** — audio-as-thread: lift `AUDIO_EXEC`'s `InterruptExecutor` /
  `SendSpawner` onto a real `std::thread` (`.make_send()`), extending this
  task's exercise to cover the priority-0 audio-task handoff path that was
  explicitly out of scope here.
- **M5** — a `loom` adapter alongside (not replacing) this TSan harness for
  exhaustive small-schedule model-checking, plus golden-master audio
  regression running all the way through the Embassy scheduler on host.
- Spikes flagged by the scoping doc: the "compiles ≠ runs" HAL audit (M2/M3),
  the host-vs-device ABI round-trip (M3), and the `loom`-vs-`platform-std` fit
  (M5).
