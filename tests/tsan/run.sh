#!/usr/bin/env bash
# Build + run the ThreadSanitizer stress harness for deluge::util::Published<T>
# and deluge::util::SpscRing<E, N> (the "publish spine" primitives). This is a
# STANDALONE TSan binary, deliberately not wired into ./dbt test -- see the
# banner comment in snapshot_primitive_stress.cpp for why.
#
# Usage:
#   tests/tsan/run.sh          # build once, run 10 times (default)
#   tests/tsan/run.sh 20       # build once, run 20 times
#
# CXX defaults to clang++ (required: TSan needs clang or a TSan-capable GCC).
#
# Exit status is nonzero if the build fails, any run exits nonzero, or any
# run reports a ThreadSanitizer finding (TSAN_OPTIONS=halt_on_error=1 below
# makes a report abort the process, which a nonzero exit already catches).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC="${SCRIPT_DIR}/snapshot_primitive_stress.cpp"
BIN="$(mktemp -t snap_stress.XXXXXX)"
RUNS="${1:-10}"
CXX="${CXX:-clang++}"

cleanup() { rm -f "${BIN}"; }
trap cleanup EXIT

echo "== building (${CXX} -fsanitize=thread) =="
"${CXX}" -std=c++23 -fsanitize=thread -O1 -g \
    -I "${REPO_ROOT}/src" -pthread \
    "${SRC}" -o "${BIN}"

echo "== confirming the binary is actually TSan-instrumented =="
tsan_syms="$(nm "${BIN}" 2>/dev/null | grep -c __tsan || true)"
if [ "${tsan_syms}" -eq 0 ]; then
    echo "ERROR: no __tsan symbols found in ${BIN} -- not actually instrumented" >&2
    exit 1
fi
echo "  ${tsan_syms} __tsan symbols linked -- a clean run below is a real negative"

fail=0
for i in $(seq 1 "${RUNS}"); do
    echo "== run ${i}/${RUNS} =="
    if ! TSAN_OPTIONS="halt_on_error=1" "${BIN}"; then
        echo "run ${i} FAILED (nonzero exit or TSan report)" >&2
        fail=1
    fi
done

if [ "${fail}" -ne 0 ]; then
    echo "== STRESS HARNESS: at least one run FAILED =="
    exit 1
fi
echo "== STRESS HARNESS: all ${RUNS} runs clean =="
