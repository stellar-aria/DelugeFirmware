#!/usr/bin/env bash
# Lens 2 (Task 9) harness runner: audio-preempts-streaming race check under ThreadSanitizer.
#
# Runs the SAME `host_app` scenario path Task 5 wired into `main.rs` (real song load, real-
# time playback on its own preemptive OS-thread executor, a concurrent output recording) —
# see main.rs's `host_app` boot block and scenario.rs's module doc — against the TSan-
# instrumented `deluge_app` (Spike B's recipe, HOST_HARNESS.md's "M4c" section), for N
# iterations, and categorizes every TSan `SUMMARY:` line it sees against the two catalogs
# in this directory:
#   - known_patterns.txt          — pre-existing, unrelated-to-streaming debt, matched by
#                                    broad file/subsystem pattern (Spike B / M4c's original
#                                    catalog plus this task's own fuller findings — see that
#                                    file's header for why patterns, not exact lines).
#   - open_findings_races.txt     — real NEW races this task found on the cluster/loaded/
#                                    recorder state itself (documented, not suppressed, not
#                                    fixed — see task-9-report.md).
# Anything matching NEITHER catalog is reported as UNCATALOGUED and fails the run — that is
# the thing this script exists to protect: a genuinely new streaming/priority/flip race must
# never silently hide behind the known noise above.
#
# Usage:
#   ./run.sh                      # build + 5 runs, default Cordae fixture, 500 blocks
#   NUM_RUNS=3 BLOCKS=2000 ./run.sh
#   DELUGE_SD_IMAGE=/path/to/cached.img ./run.sh   # skip repacking the FAT image
#   SKIP_BUILD=1 ./run.sh         # reuse an already-built TSan tree + binary (fast iteration)
#
# Prerequisites (see HOST_HARNESS.md's M4c section for the one-time setup):
#   - clang/clang++ whose LLVM major matches `rustc +nightly`'s bundled LLVM.
#   - the sysroot symlink for librustc-nightly_rt.tsan.a under the custom
#     x86_64-unknown-linux-gnu-tsan target (HOST_HARNESS.md, "One-time environment setup").
#   - mtools (mformat/mcopy) if DELUGE_SD_IMAGE isn't pre-set and no cached fixture exists.
#
# Known gotcha (Task 9's own verification pass hit this): each unset-DELUGE_SD_IMAGE
# invocation packs a fresh ~2.5GB /tmp/deluge-streaming-scenario-*.img and leaves it behind.
# Repeated invocations without cleanup can exhaust a tmpfs /tmp; separately (independent of
# free space), packing an image and consuming it as the SD backing store WITHIN THE SAME
# process was observed to be unreliable on this host — the scenario would report
# `load_dispatched=false` (currentSong never got set during boot) on every run. Reusing an
# already-packed image via `DELUGE_SD_IMAGE=...` (packed by a PRIOR, separate process) was
# 100% reliable across dozens of runs. If a run reports `load_dispatched=false` with
# `boot_ready=true`, prefer the cached-image form over debugging boot ordering — and
# periodically `rm -f /tmp/deluge-streaming-scenario-*.img` between sessions.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUST_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"          # src/bsp/rust
REPO_ROOT="$(cd "$RUST_DIR/../../.." && pwd)"
TSAN_BUILD_DIR="${TSAN_BUILD_DIR:-$REPO_ROOT/build-embassy-hostapp-tsan}"

NUM_RUNS="${NUM_RUNS:-5}"
BLOCKS="${BLOCKS:-500}"
FIXTURE="${DELUGE_STREAMING_SCENARIO_FIXTURE:-cordae}"
SONG="${DELUGE_STREAMING_SCENARIO_SONG:-SONGS/Cordae.XML}"
LOG_DIR="${LOG_DIR:-$SCRIPT_DIR/logs}"
mkdir -p "$LOG_DIR"

if [[ -z "${SKIP_BUILD:-}" ]]; then
    echo "== [1/3] Building TSan deluge_app tree ($TSAN_BUILD_DIR) =="
    cmake -B "$TSAN_BUILD_DIR" -S "$REPO_ROOT/sim" -G Ninja \
        -DDELUGE_SIM_X64=ON \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="-fshort-enums -fsanitize=thread" \
        -DCMAKE_CXX_FLAGS="-fshort-enums -fsanitize=thread" >/dev/null
    ninja -C "$TSAN_BUILD_DIR" deluge_app fatfs NE10 eyalroz_printf \
        deluge_dsp deluge_scheduler deluge_foundation deluge_midi

    echo "== [2/3] Building the Rust TSan binary (host_app) =="
    cd "$RUST_DIR"
    # Belt-and-braces staleness workaround (HOST_HARNESS.md): always wipe the tsan target
    # dir before relinking against a freshly (re)built CMake tree.
    rm -rf target/x86_64-unknown-linux-gnu-tsan
    DELUGE_HOSTAPP_BUILD_DIR="$TSAN_BUILD_DIR" \
    RUSTFLAGS="-Zsanitizer=thread" TSAN_OPTIONS="halt_on_error=0" \
    cargo +nightly build --features host_app \
        -Zbuild-std=core,alloc,std,panic_abort \
        -Zjson-target-spec \
        --target sanitizer/x86_64-unknown-linux-gnu-tsan.json
else
    echo "== [1-2/3] SKIP_BUILD set — reusing existing TSan tree + binary =="
fi

BIN="$RUST_DIR/target/x86_64-unknown-linux-gnu-tsan/debug/deluge-rust"
[[ -x "$BIN" ]] || { echo "missing $BIN — build failed?"; exit 1; }

# One TSan runtime / no undefined tsan symbols sanity check (Spike B's own check).
TSAN_INITS=$(nm "$BIN" | grep -c ' T __tsan_init' || true)
UNDEF_TSAN=$(nm -D "$BIN" | grep -c tsan || true)
echo "== TSan link sanity: __tsan_init defs=$TSAN_INITS, undefined tsan symbols=$UNDEF_TSAN =="
if [[ "$TSAN_INITS" != "1" || "$UNDEF_TSAN" != "0" ]]; then
    echo "!! unexpected TSan link shape — see HOST_HARNESS.md's clang/rustc LLVM version check"
    exit 1
fi

normalize() {
    # A TSan SUMMARY line -> the catalog key: strip repo-root / rustup-sysroot prefixes,
    # the trailing (BuildId: ...), and any (path+0xNNNN) address annotation, so the same
    # logical site matches across machines/builds/ASLR.
    sed -E \
        -e "s#$REPO_ROOT/##g" \
        -e 's#/home/[^/]+/\.rustup/toolchains/[^/]+/lib/rustlib/src/rust/##g' \
        -e 's# \(BuildId: [^)]*\)##g' \
        -e 's#\([^)]*deluge-rust\+0x[0-9a-f]+\)##g'
}

echo "== [3/3] Running $NUM_RUNS iteration(s): song=$SONG blocks=$BLOCKS fixture=$FIXTURE =="
total_known=0
total_open=0
total_uncatalogued=0
any_scenario_fail=0
uncatalogued_lines_file="$(mktemp)"

for i in $(seq 1 "$NUM_RUNS"); do
    log="$LOG_DIR/run_${i}.log"
    (
        cd "$RUST_DIR"
        env \
            ${DELUGE_SD_IMAGE:+DELUGE_SD_IMAGE="$DELUGE_SD_IMAGE"} \
            DELUGE_STREAMING_SCENARIO_FIXTURE="$FIXTURE" \
            DELUGE_STREAMING_SCENARIO_SONG="$SONG" \
            DELUGE_STREAMING_SCENARIO_BLOCKS="$BLOCKS" \
            RUST_LOG=info \
            TSAN_OPTIONS="halt_on_error=0" \
            timeout 300 "$BIN"
    ) >"$log" 2>&1 || true

    if grep -q "HOST APP scenario PASSED" "$log"; then
        scenario_status="PASSED"
    else
        scenario_status="FAILED"
        any_scenario_fail=1
    fi

    summaries="$(grep '^SUMMARY: ThreadSanitizer:' "$log" | sed -E 's/^SUMMARY: ThreadSanitizer: //' | normalize | sort -u || true)"
    known=0 open=0 uncat=0
    # Two-tier classification, checked in this order:
    #   1. open_findings_races.txt — exact-line matches for the specific, narrow, REAL
    #      cluster/loaded/recorder/resource-manager races this task found (kept visible
    #      every run, not suppressed — see that file's header).
    #   2. known_patterns.txt      — broad file/subsystem regex patterns for the pre-existing
    #      "cooperative-only, not preemption-safe" debt class, so a fresh run's different
    #      exact line (new field, same subsystem) doesn't fall through as UNCATALOGUED — see
    #      that file's header for why exact-line-only matching doesn't scale here.
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        if grep -Eq -f <(grep -Ev '^\s*(#|$)' "$SCRIPT_DIR/open_findings_races.txt") <<<"$line" 2>/dev/null; then
            open=$((open + 1))
        elif grep -Eq -f <(grep -Ev '^\s*(#|$)' "$SCRIPT_DIR/known_patterns.txt") <<<"$line" 2>/dev/null; then
            known=$((known + 1))
        else
            uncat=$((uncat + 1))
            echo "$line" >>"$uncatalogued_lines_file"
        fi
    done <<<"$summaries"

    total_known=$((total_known + known))
    total_open=$((total_open + open))
    total_uncatalogued=$((total_uncatalogued + uncat))
    echo "run $i: scenario=$scenario_status  known=$known open_findings=$open UNCATALOGUED=$uncat  (log: $log)"
done

echo
echo "== Summary over $NUM_RUNS run(s) =="
echo "  known (pre-existing, unrelated) race-site hits: $total_known"
echo "  open-finding (real, streaming/cluster-related, documented) race-site hits: $total_open"
echo "  UNCATALOGUED race-site hits: $total_uncatalogued"

status=0
if [[ "$any_scenario_fail" != "0" ]]; then
    echo "!! at least one run's scenario itself FAILED (not just a TSan finding) — see the logs"
    status=1
fi
if [[ "$total_uncatalogued" != "0" ]]; then
    echo "!! UNCATALOGUED race site(s) — a signature not in EITHER catalog. Investigate before" \
         "assuming this is more of the known noise:"
    sort -u "$uncatalogued_lines_file" | sed 's/^/    /'
    status=1
fi
rm -f "$uncatalogued_lines_file"

if [[ "$status" == "0" ]]; then
    echo "OK: every TSan finding across $NUM_RUNS run(s) matched a catalogued signature" \
         "(known-unrelated or already-documented open finding); no new streaming/cluster race."
fi
exit "$status"
