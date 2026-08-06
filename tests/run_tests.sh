#!/usr/bin/env bash
# tests/run_tests.sh — build + KAT + invariant tests for the memory-challenge PoC.
#
# Must be run inside the Docker container via:
#   ./run.sh bash tests/run_tests.sh
# from the repo root.  The container provides:
#   - /work  : the repo root (mounted)
#   - /ram   : guaranteed-RAM tmpfs
#   - IPC_LOCK capability so mlock() succeeds for the honest prover

set -euo pipefail

PASS_COUNT=0
FAIL_COUNT=0
FAILED_TESTS=()

pass() { echo "PASS: $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { echo "FAIL: $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); FAILED_TESTS+=("$1"); }

# ---------------------------------------------------------------------------
# Step 1: build binaries and KAT
# ---------------------------------------------------------------------------
cd /work

echo "=== BUILD ==="
make
g++ -O3 -std=c++17 -I/work/src -o tests/kat tests/kat.cpp
echo "Build complete."

# ---------------------------------------------------------------------------
# Step 2: KAT
# ---------------------------------------------------------------------------
echo ""
echo "=== KAT (known-answer test) ==="
if tests/kat; then
    pass "KAT"
else
    fail "KAT"
fi

# ---------------------------------------------------------------------------
# Step 3: INVARIANT tests
# ---------------------------------------------------------------------------
echo ""
echo "=== INVARIANT TESTS ==="

# Common verifier flags for all invariant runs.
# Small: 8 MiB buffer, 2048 samples, 16 lanes, 100 measured, 20 warmup.
VERIFIER_ARGS="--mb 8 --k 2048 --lanes 16 --n 100 --warmup 20 --csv /dev/null"
JSON=/tmp/inv.json

# Helper: read errors and samples from the JSON written by the verifier.
# Outputs: "ERRORS SAMPLES"
read_json() {
    python3 -c "
import json, sys
d = json.load(open('$JSON'))
r = d['result']
print(r['errors'], r['samples'])
"
}

# Helper: run one invariant case.
# Usage: run_invariant <name> <port> <server_cmd_array> <errors_check> <samples_check>
# errors_check / samples_check are python3 expressions evaluated with
# variables e (errors) and s (samples).
run_invariant() {
    local name="$1"
    local port="$2"
    local errors_ok_expr="$3"  # python3 bool expression using e, s
    shift 3
    # Remaining args are the server command.
    local server_cmd=("$@")

    echo ""
    echo "--- INVARIANT: $name ---"

    # Start server in background.
    "${server_cmd[@]}" >/tmp/inv_server_${name}.log 2>&1 &
    local srv_pid=$!

    # Give the server a moment to bind.
    sleep 0.6

    local verifier_ok=1
    ./verifier --port "$port" $VERIFIER_ARGS --json "$JSON" >/tmp/inv_verifier_${name}.log 2>&1 || verifier_ok=0

    # Kill the server.
    kill -9 "$srv_pid" 2>/dev/null || true
    wait "$srv_pid" 2>/dev/null || true

    if [ "$verifier_ok" -ne 1 ]; then
        echo "  Verifier failed to connect or run. Server log:"
        cat /tmp/inv_server_${name}.log || true
        fail "INVARIANT $name"
        return
    fi

    # Parse JSON.
    local result
    result=$(read_json) || { fail "INVARIANT $name (json parse error)"; return; }
    local errors samples
    read -r errors samples <<< "$result"

    # Evaluate the correctness assertion.
    local ok
    ok=$(python3 -c "
e = $errors
s = $samples
result = bool($errors_ok_expr)
print('1' if result else '0')
")

    if [ "$ok" = "1" ]; then
        pass "INVARIANT $name (errors=$errors/samples=$samples)"
    else
        fail "INVARIANT $name (errors=$errors/samples=$samples)"
        echo "  Expected: $errors_ok_expr"
    fi
}

# --- Case 1: honest prover — errors must be 0 ---
run_invariant "honest" 9101 \
    "e == 0" \
    ./prover 9101

# --- Case 2: cheating_prover disk mode — correct answers, errors must be 0 ---
run_invariant "disk" 9102 \
    "e == 0" \
    ./cheating_prover --port 9102 --mode disk --datafile /tmp/memchal_test_disk.bin

# --- Case 3: cheating_prover mmapdisk mode — correct answers, errors must be 0 ---
run_invariant "mmapdisk" 9103 \
    "e == 0" \
    ./cheating_prover --port 9103 --mode mmapdisk --datafile /tmp/memchal_test_mmap.bin

# --- Case 4: cheating_prover fake mode with frac=0.5 — ALL answers wrong ---
# fake fabricates misses, so errors should equal samples and samples > 0.
run_invariant "fake" 9104 \
    "e == s and s > 0" \
    ./cheating_prover --port 9104 --mode fake --frac 0.5 --datafile /tmp/memchal_test_fake.bin

# ---------------------------------------------------------------------------
# Step 4: report formatting — the experiment tables are read off a projector, so
# a frame that doesn't close is a real defect. Every rendered line must be exactly
# the same display width, including content with typographic characters.
# ---------------------------------------------------------------------------
echo ""
echo "=== INVARIANT: report table alignment ==="
if python3 - <<'PYEOF'
import sys
sys.path.insert(0, "/work/scripts")
from fmt_table import table, block

cases = [
    table([("scenario", "l"), ("p50 (us)", "r")],
          [["honest — mlock'd", "42.2"], ["mmapdisk ≈ honest", "1,451.0"]],
          title="A TITLE LONGER THAN THE COLUMNS PUT TOGETHER",
          notes=["a note — with an em dash"]),
    table([("a", "l"), ("b", "r")], [["x", "1"]]),
    block("VERDICT — SHORT TITLE", ["a line", "a much longer line — with a dash", ""]),
]
FRAME = set("═║╔╗╚╝╠╣╦╩╬╟╢┼─")
bad = 0
for i, rendered in enumerate(cases):
    lines = [l for l in rendered.split("\n") if l[:1] in FRAME]
    widths = {len(l) for l in lines}
    if len(widths) != 1:
        bad += 1
        print(f"  case {i}: ragged frame, line widths {sorted(widths)}")
    # Content must be folded to ASCII; only the frame itself may be non-ASCII,
    # since ambiguous-width glyphs are what break alignment on other terminals.
    stray = {c for l in lines for c in l if ord(c) > 127 and c not in FRAME}
    if stray:
        bad += 1
        print(f"  case {i}: non-ASCII content survived folding: {sorted(stray)}")
print("  all frames closed, content is ASCII" if not bad else f"  {bad} case(s) bad")
sys.exit(1 if bad else 0)
PYEOF
then
    pass "INVARIANT report alignment"
else
    fail "INVARIANT report alignment"
fi

# ---------------------------------------------------------------------------
# Final summary
# ---------------------------------------------------------------------------
echo ""
echo "=============================="
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "ALL TESTS PASSED ($PASS_COUNT passed, 0 failed)"
    exit 0
else
    echo "TESTS FAILED: $FAIL_COUNT failed, $PASS_COUNT passed"
    echo "Failed tests:"
    for t in "${FAILED_TESTS[@]}"; do
        echo "  - $t"
    done
    exit 1
fi
