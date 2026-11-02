#!/usr/bin/env bash
# tests/run_tests.sh — build + known-answer + invariant tests for the memory-challenge PoC.
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

# Wait until a TCP listener is accepting on 127.0.0.1:<port> (up to ~10s). The
# provers print "listening on" and bind before their accept loop, so a fixed
# sleep is a race on both ends: too short and the verifier fails to connect, too
# long and every case pays for it. Polling the port is faster when the server is
# quick and reliable when the machine is loaded. A probe connection that sends
# nothing is harmless — the server logs a short/bad hello and loops back to
# accept the real verifier.
wait_for_port() {
    local port="$1" waited=0
    while ! (echo >"/dev/tcp/127.0.0.1/$port") 2>/dev/null; do
        sleep 0.05
        waited=$((waited + 1))
        [ "$waited" -gt 200 ] && { echo "  ERROR: no listener on port $port after ~10s" >&2; return 1; }
    done
    return 0
}

# ---------------------------------------------------------------------------
# Step 1: build binaries and the known-answer tests
# ---------------------------------------------------------------------------
cd /work

echo "=== BUILD ==="
make
make tests
echo "Build complete."

# ---------------------------------------------------------------------------
# Step 2: known-answer tests
# ---------------------------------------------------------------------------
echo ""
echo "=== KNOWN-ANSWER TEST (challenge math) ==="
if tests/known_answer; then
    pass "known-answer test"
else
    fail "known-answer test"
fi

# The buffer generator is pinned separately, against vectors published by other
# people (RFC 7693 for Blake2b, RFC 9106 for Argon2d). A failure here means the
# generator is no longer Argon2d, so ADR 0007's analysis no longer applies to it.
echo ""
echo "=== KNOWN-ANSWER TEST (buffer generator) ==="
if tests/known_answer_argon2d; then
    pass "known-answer test (generator)"
else
    fail "known-answer test (generator)"
fi

# The published vectors pin one point each. This sweeps the parameter space
# against the reference implementation, so a bug that the RFC vector happens to
# miss still shows up. libargon2 is a test-only dependency.
echo ""
echo "=== DIFFERENTIAL TEST (generator vs libargon2) ==="
if tests/differential_argon2d; then
    pass "differential test (generator)"
else
    fail "differential test (generator)"
fi

# The partial-recompute adversary rebuilds discarded blocks from the recipe. If
# its rebuilt bytes differ from the real matrix, every timing it reports is for
# a cheat that answers wrong, so this gate comes before any GPU number.
echo ""
echo "=== REBUILD TEST (partial-recompute engine) ==="
if tests/rebuild_argon2d; then
    pass "rebuild test (partial-recompute engine)"
else
    fail "rebuild test (partial-recompute engine)"
fi

# ---------------------------------------------------------------------------
# Step 3: INVARIANT tests
# ---------------------------------------------------------------------------
echo ""
echo "=== INVARIANT TESTS ==="

# Common verifier flags for all invariant runs.
# Small: 8 MiB buffer, 2048 samples, 16 lanes, 100 measured, 20 warmup.
# The verifier now defaults to the secure config (--gen chacha --chain dependent
# --nonce random). These base cases pin --chain independent so the independent
# path stays covered here while Step 3b covers the dependent path; the gen/nonce-
# specific cases below name their generator/nonce explicitly rather than lean on
# the default, so the suite is stable whichever way the binary defaults point.
VERIFIER_ARGS="--mb 8 --k 2048 --lanes 16 --n 100 --warmup 20 --csv /dev/null --chain independent"
JSON=/tmp/inv.json

# Helper: run one invariant case.
# Usage: run_invariant <name> <port> <expect> <server command...>
#   expect: "correct"   — every answer right (errors == 0)
#           "all_wrong" — every answer wrong (errors == samples)
# Both also require at least one sample. Set VERIFIER_EXTRA for flags that
# apply to this case only.
run_invariant() {
    local name="$1"
    local port="$2"
    local expect="$3"
    shift 3
    # Remaining args are the server command.
    local server_cmd=("$@")

    echo ""
    echo "--- INVARIANT: $name ---"

    # Start server in background.
    "${server_cmd[@]}" >/tmp/inv_server_${name}.log 2>&1 &
    local srv_pid=$!

    # Wait for the server to bind before connecting.
    wait_for_port "$port"

    local verifier_ok=1
    ./verifier --port "$port" $VERIFIER_ARGS ${VERIFIER_EXTRA:-} --json "$JSON" >/tmp/inv_verifier_${name}.log 2>&1 || verifier_ok=0

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
    result=$(python3 tests/verifier_result.py "$JSON") || { fail "INVARIANT $name (json parse error)"; return; }
    local errors samples
    read -r errors samples <<< "$result"

    local ok=0
    case "$expect" in
        correct)   [ "$samples" -gt 0 ] && [ "$errors" -eq 0 ] && ok=1 ;;
        all_wrong) [ "$samples" -gt 0 ] && [ "$errors" -eq "$samples" ] && ok=1 ;;
        *) echo "  unknown expectation: $expect" ;;
    esac

    if [ "$ok" = "1" ]; then
        pass "INVARIANT $name (errors=$errors/samples=$samples)"
    else
        fail "INVARIANT $name (errors=$errors/samples=$samples)"
        echo "  Expected: $expect"
    fi
}

# --- Case 1: honest prover — errors must be 0 ---
run_invariant "honest" 9101 \
    correct \
    ./prover 9101

# --- Case 2: cheating_prover disk mode — correct answers, errors must be 0 ---
run_invariant "disk" 9102 \
    correct \
    ./cheating_prover --port 9102 --mode disk --datafile /tmp/memchal_test_disk.bin

# --- Case 3: cheating_prover mmapdisk mode — correct answers, errors must be 0 ---
run_invariant "mmapdisk" 9103 \
    correct \
    ./cheating_prover --port 9103 --mode mmapdisk --datafile /tmp/memchal_test_mmap.bin

# --- Case 4: cheating_prover fake mode with frac=0.5 — ALL answers wrong ---
# fake fabricates misses, so errors should equal samples and samples > 0.
run_invariant "fake" 9104 \
    all_wrong \
    ./cheating_prover --port 9104 --mode fake --frac 0.5 --datafile /tmp/memchal_test_fake.bin

# --- Build-buffer path (--gen argon2d): the verifier sends a recipe instead of
# the buffer and both sides build it. The prover's independently built copy has
# to agree with the verifier's on every challenge, so 0 errors here also proves
# the two builds are byte-identical. fake still fabricates, so it still fails
# every challenge — the correctness detector is unaffected by where bytes came
# from, which is the point of leaving the challenge math alone.
_SAVE_ARGS="$VERIFIER_ARGS"; VERIFIER_ARGS="$VERIFIER_ARGS --gen argon2d"

run_invariant "build_honest" 9121 \
    correct \
    ./prover 9121

run_invariant "build_cheat_honest" 9122 \
    correct \
    ./cheating_prover --port 9122 --mode honest

run_invariant "build_fake" 9123 \
    all_wrong \
    ./cheating_prover --port 9123 --mode fake --frac 0.5 --datafile /tmp/memchal_build_fake.bin

run_invariant "build_mmapdisk" 9124 \
    correct \
    ./cheating_prover --port 9124 --mode mmapdisk --datafile /tmp/memchal_build_mmap.bin

VERIFIER_ARGS="$_SAVE_ARGS"

# --- Case 5: cheating_prover prefetch mode — correct answers (timing attack only), errors must be 0 ---
run_invariant "prefetch" 9109 \
    correct \
    ./cheating_prover --port 9109 --mode prefetch --datafile /tmp/memchal_test_prefetch.bin

# --- Case 6: recompute vs splitmix64 verifier — attack SUCCEEDS, 0 errors ---
# The recompute adversary recovers the splitmix64 seed from buf[0] and
# regenerates every word in O(1) with ~zero memory.  Against a splitmix64
# buffer the regenerated data is correct -> 0 errors (attack wins).
# splitmix64 is the weak generator, so name it explicitly (the default is chacha).
VERIFIER_EXTRA="--gen splitmix64" run_invariant "recompute_vs_splitmix" 9106 \
    correct \
    ./cheating_prover --port 9106 --mode recompute --datafile /tmp/memchal_test_recompute.bin

# --- Case 7: recompute vs chacha20 verifier — defense WORKS, ALL errors ---
# With a chacha20-generated buffer the recompute adversary cannot recover the
# key from one output word.  Its "recovered seed" is bogus -> wrong words ->
# every answer is wrong -> errors == samples (defense wins).
VERIFIER_EXTRA="--gen chacha" run_invariant "recompute_vs_chacha20" 9108 \
    all_wrong \
    ./cheating_prover --port 9108 --mode recompute --datafile /tmp/memchal_test_recompute3.bin

# --- Case 8: honest prover vs chacha20 buffer — errors must be 0 ---
# Verify that the honest prover (which receives the buffer via the upload
# protocol and reads it directly) still produces 0 errors when the verifier
# fills the buffer with a random 256-bit key via --gen chacha.  This pins the
# handshake between the new fill_buffer_chacha path and the prover's answer.
VERIFIER_EXTRA="--gen chacha" run_invariant "honest_vs_chacha" 9112 \
    correct \
    ./prover 9112

# --- Case 9: precompute vs sequential nonces — attack SUCCEEDS, 0 errors ---
# The precompute adversary caches answers from predicted nonces; against
# sequential nonces its predictions are all correct -> 0 verification errors.
# sequential is the weak nonce mode, so name it explicitly (the default is random).
VERIFIER_EXTRA="--nonce sequential" run_invariant "precompute_vs_sequential" 9110 \
    correct \
    ./cheating_prover --port 9110 --mode precompute --datafile /tmp/memchal_test_precompute_seq.bin

# --- Case 10: precompute vs random nonces — correctness must still hold ---
# Under random nonces the predictions miss -> the prover computes from disk on
# the spot (slow, caught by timing). But correctness must still be 0 errors
# because it reads real data even on cache misses.
VERIFIER_EXTRA="--nonce random" run_invariant "precompute_vs_random" 9111 \
    correct \
    ./cheating_prover --port 9111 --mode precompute --datafile /tmp/memchal_test_precompute_rnd.bin

# ---------------------------------------------------------------------------
# Step 3b: INVARIANT tests under --chain dependent
# All correctness invariants must hold under the chained path too:
#   honest/disk/mmapdisk/prefetch -> 0 errors; fake -> all errors.
# Recompute is not tested here because the chain mode has no bearing on the
# key-recovery math (the verifier is what changes behavior).
# ---------------------------------------------------------------------------
echo ""
echo "=== INVARIANT TESTS (--chain dependent) ==="

# Temporarily extend VERIFIER_ARGS with --chain dependent so the verifier
# sends kFlagChained and computes the expected answer with the chained path.
_SAVE_VERIFIER_ARGS="$VERIFIER_ARGS"
VERIFIER_ARGS="$VERIFIER_ARGS --chain dependent"

run_invariant "chain_dep_honest" 9201 \
    correct \
    ./prover 9201

run_invariant "chain_dep_disk" 9202 \
    correct \
    ./cheating_prover --port 9202 --mode disk --datafile /tmp/memchal_test_dep_disk.bin

run_invariant "chain_dep_mmapdisk" 9203 \
    correct \
    ./cheating_prover --port 9203 --mode mmapdisk --datafile /tmp/memchal_test_dep_mmap.bin

run_invariant "chain_dep_prefetch" 9204 \
    correct \
    ./cheating_prover --port 9204 --mode prefetch --datafile /tmp/memchal_test_dep_prefetch.bin

run_invariant "chain_dep_fake" 9205 \
    all_wrong \
    ./cheating_prover --port 9205 --mode fake --frac 0.5 --datafile /tmp/memchal_test_dep_fake.bin

VERIFIER_ARGS="$_SAVE_VERIFIER_ARGS"

# ---------------------------------------------------------------------------
# Step 4: live demo instrumentation. The browser demo draws its page map from two
# independent base64 bitstreams; a broken encoding would draw a convincing, wrong
# picture rather than fail, so the invariants are tested here.
# ---------------------------------------------------------------------------
echo ""
echo "=== INVARIANT: live stream (demo instrumentation) ==="
STREAM_DIR=$(mktemp -d /tmp/memchal_stream_XXXXXX)
./cheating_prover --port 9105 --mode mmapdisk --datafile /tmp/memchal_test_stream.bin \
    --trace 10 >"$STREAM_DIR/prover.log" 2>&1 &
STREAM_PID=$!
wait_for_port 9105
./verifier --port 9105 --mb 8 --k 512 --lanes 8 --n 30 --warmup 10 \
    --csv /dev/null --stream "$STREAM_DIR/verifier.jsonl" --probe-map 10 >/dev/null 2>&1 || true
# Not a bind wait: the prover emits its trace events AFTER each response (outside
# the timed window), so give it a moment to flush the last ones to its log before
# we kill it, or check_stream.py sees a truncated trace.
sleep 0.4
kill "$STREAM_PID" 2>/dev/null || true
wait "$STREAM_PID" 2>/dev/null || true
if python3 tests/check_stream.py "$STREAM_DIR/verifier.jsonl" "$STREAM_DIR/prover.log"; then
    pass "INVARIANT live stream"
else
    fail "INVARIANT live stream"
fi
rm -rf "$STREAM_DIR"

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# The GPU demo page's canvas sizing.
#
# Not C++, and not part of the protocol — but a canvas that grows on every
# redraw took a live demo session down, and the failure mode (read back a value
# you just wrote) is invisible in review. Needs node, which the measurement
# container does not ship, so it skips there and runs on a developer machine.
# ---------------------------------------------------------------------------
echo ""
echo "=== GPU demo: canvas sizing ==="
if command -v node >/dev/null 2>&1; then
    if node "$(dirname "$0")/canvas_fit_test.js"; then
        pass "canvas fit (demo page cannot grow itself to death)"
    else
        fail "canvas fit"
    fi
else
    echo "  SKIP: node not available in this environment"
fi

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
