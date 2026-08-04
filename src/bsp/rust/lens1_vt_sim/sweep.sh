#!/usr/bin/env bash
#
# Lens 1 (streaming-underrun harness) margin sweep + negative controls: a repeatable,
# deterministic sweep of `lens1-vt-sim` over modeled SD latency (the `sim_latency`
# throughput/overhead lever — see `../src/sd.rs`'s `latency_for`), reporting the underrun
# curve and the THRESHOLD throughput at which underruns first appear (the "margin": "keeps
# up for SD latency better than L"), plus negative control A §9 of the spec requires
# as an ACCEPTANCE criterion, not just supplementary data:
#
#   Control A (detection): past the threshold, underrun_unassign > 0; at the fast/default
#     latency, == 0 — proof the harness's instrumentation actually fires.
#
# (Control B — mechanism efficacy of the HIGH-priority dispatch / recorder-drain yield —
# was retired with that worker-ring tier; the sim knobs it toggled no longer exist.)
#
# Every run is a single deterministic virtual-time simulation (no wall-clock, no RNG) —
# repeat any invocation and the numbers reproduce exactly. This script just automates
# running `lens1-vt-sim` several times as
# separate PROCESSES (not an in-process loop): the C++ app's static/global state
# (`currentSong`, the storage owner, ...) is never designed to be reset and re-driven twice
# in one process lifetime, so a fresh process per data point is the safe, simple shape —
# matching every other point in this harness (main.rs's own `hard_exit` skips C++ static
# destructors on the assumption the process exits once).
#
# Usage:
#   sweep.sh                 # build (unless NO_BUILD=1), run the margin sweep + control A
#   sweep.sh margin           # margin sweep only
#   sweep.sh control-a        # negative control A only
#
# Env overrides:
#   FIXTURE          song fixture                              (default: cordae)
#   BLOCKS           audio blocks per run (~3ms/block)          (default: 20000, ~58s virtual)
#   OVERHEAD_US       fixed sim_latency command overhead         (default: 500)
#   NO_BUILD=1       skip `cargo build --release`
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/target/release/lens1-vt-sim"

FIXTURE="${FIXTURE:-cordae}"
BLOCKS="${BLOCKS:-20000}"
OVERHEAD_US="${OVERHEAD_US:-500}"
CMD="${1:-all}"

if [[ "${NO_BUILD:-0}" != "1" ]]; then
    echo "sweep.sh: cargo build --release" >&2
    (cd "$HERE" && cargo build --release --quiet)
fi

# Runs one lens1-vt-sim invocation with the given extra env (as NAME=VALUE args) and prints
# its LENS1_RESULT line's fields as a single space-joined "key=value ..." string on stdout.
# Fails loudly (set -e) if the binary doesn't reach a clean LENS1_RESULT line — a wedged or
# crashed run is a harness bug, not a data point to silently drop from the sweep.
run_point() {
    local extra_env=("$@")
    local out
    out=$(env "${extra_env[@]}" \
        LENS1_FIXTURE="$FIXTURE" LENS1_BLOCKS="$BLOCKS" LENS1_STEP_TIMEOUT_S=120 \
        LENS1_VIRTUAL_BUDGET_MS=600000 "$BIN" 2>/dev/null | grep '^LENS1_RESULT')
    if [[ -z "$out" ]]; then
        echo "sweep.sh: run_point FAILED (no LENS1_RESULT line) for env: ${extra_env[*]}" >&2
        exit 1
    fi
    echo "$out"
}

field() { # field <LENS1_RESULT line> <name>
    echo "$1" | grep -oP "(?<=$2=)[0-9]+"
}

# --- Margin sweep --------------------------------------------------------------------
# Geometric-ish descent from a fast/default-plausible throughput down to the extreme low
# end, denser in the 300k-2M band where the curve is expected to cross from 0 to positive,
# to pin the threshold tightly.
MARGIN_THROUGHPUTS=(2000000 1500000 1000000 800000 600000 500000 400000 300000 200000 150000 100000 50000 20000 10000 5000)

run_margin_sweep() {
    echo "=== Margin sweep: fixture=$FIXTURE blocks=$BLOCKS overhead_us=$OVERHEAD_US ===" >&2
    local threshold="" prev_bps="" prev_unassign=0
    printf '%-12s %-14s %-14s\n' "throughput_bps" "underrun_wait" "underrun_unassign"
    for bps in "${MARGIN_THROUGHPUTS[@]}"; do
        local line unassign wait_c
        line=$(run_point LENS1_THROUGHPUT_BPS="$bps" LENS1_OVERHEAD_US="$OVERHEAD_US")
        wait_c=$(field "$line" underrun_wait)
        unassign=$(field "$line" underrun_unassign)
        printf '%-12s %-14s %-14s\n' "$bps" "$wait_c" "$unassign"
        if [[ "$unassign" -gt 0 && -z "$threshold" ]]; then
            threshold="between ${bps} and ${prev_bps:-N/A} bytes/sec (first nonzero at ${bps}, last zero at ${prev_bps:-N/A})"
        fi
        prev_bps="$bps"
        prev_unassign="$unassign"
    done
    echo >&2
    if [[ -n "$threshold" ]]; then
        echo "MARGIN_THRESHOLD: $threshold (overhead_us=$OVERHEAD_US)" >&2
    else
        echo "MARGIN_THRESHOLD: no crossing observed in swept range" >&2
    fi
}

# --- Negative control A (detection) --------------------------------------------------
run_control_a() {
    echo "=== Negative control A: detection ===" >&2
    local fast slow fast_u slow_u
    fast=$(run_point LENS1_THROUGHPUT_BPS=2000000 LENS1_OVERHEAD_US="$OVERHEAD_US")
    slow=$(run_point LENS1_THROUGHPUT_BPS=5000 LENS1_OVERHEAD_US=2000)
    fast_u=$(field "$fast" underrun_unassign)
    slow_u=$(field "$slow" underrun_unassign)
    echo "  fast/default (2,000,000 B/s):  underrun_unassign=$fast_u  (expect 0)" >&2
    echo "  slow (5,000 B/s, 2000us):      underrun_unassign=$slow_u  (expect > 0)" >&2
    if [[ "$fast_u" -eq 0 && "$slow_u" -gt 0 ]]; then
        echo "CONTROL_A: PASS — harness detects underrun (fires only under stress)" >&2
    else
        echo "CONTROL_A: FAIL — fast=$fast_u slow=$slow_u" >&2
        exit 1
    fi
}

case "$CMD" in
    margin) run_margin_sweep ;;
    control-a) run_control_a ;;
    all)
        run_margin_sweep
        echo >&2
        run_control_a
        ;;
    *)
        echo "usage: sweep.sh [margin|control-a]" >&2
        exit 2
        ;;
esac
