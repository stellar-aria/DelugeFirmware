#!/usr/bin/env bash
#
# Embassy-vs-golden differential: renders `golden_vt_render` (the Embassy-host-BSP
# golden renderer, `src/bsp/rust/golden_vt_render/`) in MIXDOWN mode for one fixture
# and byte-compares its output against the SAME established golden baseline
# `scripts/golden_mixdown.sh` gates the C-host `deluge_render` sim against — not a
# fresh reference, the actual `<fixture>_MIXDOWN.golden.wav.sha256` in
# `DELUGE_GOLDEN_DIR`. This is the reusable convergence gate for the
# golden-on-Embassy work: run it after any change that could perturb the Embassy
# renderer's output (sim_latency/config changes, streaming-fill relocations, etc.)
# to prove the Rust/Embassy BSP still produces bit-exact golden output.
#
# Usage:
#   scripts/golden_embassy_diff.sh <fixture>      # cordae | highsiderr | icoustic
#
# Exit: 0 = PASS (byte-identical to the stored golden), 1 = mismatch or a render
# crash/wedge, 2 = setup error (no baseline yet, no corpus, etc.)
#
# Env overrides (same conventions as golden_mixdown.sh / sd_image.rs):
#   DELUGE_GOLDEN_DIR         where the reconstructed project + golden baseline live
#                             (default: ~/.cache/deluge-golden)
#   DELUGE_BACKUP             the "Deluge Backup" root, used to reconstruct a fixture's
#                             project tree the first time (default: ~/Deluge Backup)
#   BUILD_DIR                 the build-embassy-hostapp C++ object-closure build dir
#                             (default: <repo>/build-embassy-hostapp)
#   GOLDEN_VIRTUAL_BUDGET_MS  virtual-time wedge-detection budget passed through to
#                             golden_vt_render (default: 1800000 = 30 simulated minutes;
#                             cheap to over-provision — the discrete-event driver jumps
#                             straight to the next due deadline, so a large budget costs
#                             no extra wall time unless the render actually needs it)
#   NO_BUILD=1                skip the ninja/cargo rebuild steps
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/build-embassy-hostapp}"
GOLDEN_DIR="${DELUGE_GOLDEN_DIR:-$HOME/.cache/deluge-golden}"
CRATE_DIR="$REPO/src/bsp/rust/golden_vt_render"

FIXTURE="${1:-}"
case "$FIXTURE" in
cordae | highsiderr | icoustic) ;;
*)
	echo "usage: $0 <cordae|highsiderr|icoustic>"
	exit 2
	;;
esac

GOLDEN_SHA_FILE="$GOLDEN_DIR/${FIXTURE}_MIXDOWN.golden.wav.sha256"
[ -f "$GOLDEN_SHA_FILE" ] || {
	echo "ERROR: no MIXDOWN golden baseline for '$FIXTURE' yet — run:"
	echo "  FIXTURE=$FIXTURE MODE=MIXDOWN scripts/golden_mixdown.sh update"
	exit 2
}
GOLDEN_SHA="$(cut -c1-64 "$GOLDEN_SHA_FILE")"

if [ "${NO_BUILD:-0}" != 1 ]; then
	[ -d "$BUILD_DIR" ] || {
		echo "ERROR: '$BUILD_DIR' does not exist — configure build-embassy-hostapp from sim/ first"
		exit 2
	}
	# Cargo does not track the C++ sources golden_vt_render links against — force a
	# rebuild of the host object closure exactly like the plan's build reference does.
	ninja -C "$BUILD_DIR" deluge_app fatfs NE10 eyalroz_printf deluge_dsp deluge_scheduler \
		deluge_foundation deluge_midi
	(cd "$CRATE_DIR" && cargo build --release)
fi

BIN="$CRATE_DIR/target/release/golden_vt_render"
[ -x "$BIN" ] || { echo "ERROR: '$BIN' not built (see NO_BUILD)"; exit 2; }

OUT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/golden_embassy_diff.${FIXTURE}.XXXXXX")"
cleanup() { rm -rf "$OUT_DIR"; }
trap cleanup EXIT

set +e
GOLDEN_FIXTURE="$FIXTURE" \
	GOLDEN_STEM_MODE=MIXDOWN \
	GOLDEN_STEM_OUT="$OUT_DIR" \
	GOLDEN_VIRTUAL_BUDGET_MS="${GOLDEN_VIRTUAL_BUDGET_MS:-1800000}" \
	"$BIN" >"$OUT_DIR.log" 2>&1
status=$?
set -e

if [ "$status" -ne 0 ]; then
	echo "FAIL — golden_vt_render exited $status (crash or wedge) for fixture=$FIXTURE"
	echo "  log: $OUT_DIR.log"
	trap - EXIT
	exit 1
fi

wav="$(find "$OUT_DIR" -iname '*.wav' | sort | head -1)"
if [ -z "$wav" ]; then
	echo "FAIL — golden_vt_render produced no WAV for fixture=$FIXTURE (see $OUT_DIR.log)"
	trap - EXIT
	exit 1
fi

render_sha="$(sha256sum "$wav" | awk '{print $1}')"
if [ "$render_sha" = "$GOLDEN_SHA" ]; then
	echo "PASS — $FIXTURE MIXDOWN matches golden (${GOLDEN_SHA:0:16}…)"
	rm -f "$OUT_DIR.log"
	exit 0
fi

echo "FAIL — $FIXTURE MIXDOWN differs from golden"
echo "  golden sha256: $GOLDEN_SHA"
echo "  render sha256: $render_sha"
echo "  render kept for inspection: $wav (log: $OUT_DIR.log)"
trap - EXIT
exit 1
