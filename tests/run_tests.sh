#!/usr/bin/env bash
# Builds everything and runs the known-answer and end-to-end tests.
# Usage: docker/run.sh bash tests/run_tests.sh
set -euo pipefail
cd "$(dirname "$0")/.."

PASS_COUNT=0
FAIL_COUNT=0
FAILED_TESTS=()

pass() { echo "PASS: $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { echo "FAIL: $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); FAILED_TESTS+=("$1"); }

# Waits up to 10 s for a listener on 127.0.0.1:<port>.
wait_for_port() {
    local port="$1" waited=0
    while ! (echo >"/dev/tcp/127.0.0.1/$port") 2>/dev/null; do
        sleep 0.05
        waited=$((waited + 1))
        [ "$waited" -gt 200 ] && { echo "  ERROR: no listener on port $port after ~10s" >&2; return 1; }
    done
    return 0
}

# Waits up to 2 s for a line matching <pattern> in <file>.
wait_for_log() {
    local file="$1" pattern="$2" waited=0
    until grep -q "$pattern" "$file" 2>/dev/null; do
        sleep 0.05
        waited=$((waited + 1))
        [ "$waited" -gt 40 ] && return 1
    done
    return 0
}

run_unit() {
    local name="$1"; shift
    echo ""
    echo "=== $name ==="
    if "$@"; then pass "$name"; else fail "$name"; fi
}

echo "=== BUILD ==="
make all tests

run_unit "challenge known-answer test"    build/tests/shared/challenge_kat
run_unit "Argon2d known-answer test"      build/tests/shared/argon2d_kat
run_unit "dependent chain test"           build/tests/shared/chain_test
run_unit "Argon2d differential test"      build/tests/shared/argon2d_differential
run_unit "Argon2d rebuild test"           build/tests/attacks/argon2d_rebuild_test
run_unit "splitmix64 seed recovery test"  build/tests/attacks/seed_recovery_test

VERIFIER_ARGS="--mb 8 --samples 2048 --lanes 16 --challenges 100 --warmup 20 --csv /dev/null --chain independent"

# Runs a prover and a verifier against it.
#   expect: "correct" (no errors) or "all_wrong" (every answer wrong).
#   VERIFIER_EXTRA: verifier flags for this case only.
#   LOG_DONE: the prover's last log line for a session (default "served").
run_invariant() {
    local name="$1" port="$2" expect="$3"
    shift 3

    echo ""
    echo "--- INVARIANT: $name ---"
    "$@" >"/tmp/inv_server_${name}.log" 2>&1 &
    local srv_pid=$!
    if ! wait_for_port "$port"; then
        kill -9 "$srv_pid" 2>/dev/null || true
        echo "  Prover never listened. Server log:"
        cat "/tmp/inv_server_${name}.log" || true
        fail "INVARIANT $name (prover never listened)"
        return
    fi

    local json="/tmp/inv_${name}.json"
    rm -f "$json"
    local verifier_ok=1
    build/verifier --port "$port" $VERIFIER_ARGS ${VERIFIER_EXTRA:-} --json "$json" \
        >"/tmp/inv_verifier_${name}.log" 2>&1 || verifier_ok=0
    wait_for_log "/tmp/inv_server_${name}.log" "${LOG_DONE:-served}" || true
    kill -9 "$srv_pid" 2>/dev/null || true
    wait "$srv_pid" 2>/dev/null || true

    if [ "$verifier_ok" -ne 1 ]; then
        echo "  Verifier failed. Server log:"
        cat "/tmp/inv_server_${name}.log" || true
        fail "INVARIANT $name"
        return
    fi

    local errors checked
    local result
    result=$(python3 tests/verifier_result.py "$json") || { fail "INVARIANT $name (json parse error)"; return; }
    read -r errors checked <<< "$result"

    local ok=0
    case "$expect" in
        correct)   [ "$checked" -gt 0 ] && [ "$errors" -eq 0 ] && ok=1 ;;
        all_wrong) [ "$checked" -gt 0 ] && [ "$errors" -eq "$checked" ] && ok=1 ;;
    esac
    if [ "$ok" = "1" ]; then
        pass "INVARIANT $name (errors=$errors/checked=$checked)"
    else
        fail "INVARIANT $name (errors=$errors/checked=$checked, expected $expect)"
    fi
}

# Checks a number the prover printed: check_count LABEL NAME REGEX OP VALUE, where
# REGEX captures the number and OP is a test(1) comparison such as -gt.
check_count() {
    local label="$1" name="$2" regex="$3" op="$4" want="$5" got
    got=$(sed -nE "s/.*${regex}.*/\1/p" "/tmp/inv_server_${name}.log" | head -1)
    if [ -n "$got" ] && [ "$got" "$op" "$want" ]; then
        pass "$label ($got)"
    else
        fail "$label (${got:-not reported})"
    fi
}

PROVER=build/cpu/prover
CHEAT=build/attacks/cpu/cheating_prover

echo ""
echo "=== INVARIANT TESTS ==="
run_invariant honest     9101 correct   $PROVER --port 9101
run_invariant disk       9102 correct   $CHEAT --port 9102 --mode disk --datafile /tmp/memchal_test_disk.bin
run_invariant mmapdisk   9103 correct   $CHEAT --port 9103 --mode mmapdisk --datafile /tmp/memchal_test_mmap.bin
run_invariant fake       9104 all_wrong $CHEAT --port 9104 --mode fake --frac 0.5 --datafile /tmp/memchal_test_fake.bin
run_invariant prefetch   9109 correct   $CHEAT --port 9109 --mode prefetch --datafile /tmp/memchal_test_prefetch.bin
VERIFIER_EXTRA="--gen splitmix64" run_invariant recompute_vs_splitmix 9106 correct \
    $CHEAT --port 9106 --mode recompute
VERIFIER_EXTRA="--gen chacha" run_invariant recompute_vs_chacha 9108 all_wrong \
    $CHEAT --port 9108 --mode recompute
VERIFIER_EXTRA="--gen chacha" run_invariant honest_vs_chacha 9112 correct $PROVER --port 9112
LOG_DONE="io: probes" run_invariant partial 9107 correct \
    $CHEAT --port 9107 --mode partial --frac 0.5 --datafile /tmp/memchal_test_partial.bin
check_count "partial reads what it doesn't hold from disk" partial 'io: probes=([0-9]+)' -gt 0

echo ""
echo "=== ATTACKS WIN WITH THE DEFENSE OFF AND LOSE WITH IT ON ==="
VERIFIER_EXTRA="--nonce sequential --gap-us 2000" LOG_DONE="cache hits" \
    run_invariant precompute_vs_sequential 9110 correct \
    $CHEAT --port 9110 --mode precompute --datafile /tmp/memchal_test_precompute_seq.bin
check_count "precompute answers sequential nonces ahead" precompute_vs_sequential \
    'cache hits=([0-9]+) of' -ge 60
VERIFIER_EXTRA="--nonce random --gap-us 2000" LOG_DONE="cache hits" \
    run_invariant precompute_vs_random 9111 correct \
    $CHEAT --port 9111 --mode precompute --datafile /tmp/memchal_test_precompute_rnd.bin
check_count "precompute answers no random nonce ahead" precompute_vs_random \
    'cache hits=([0-9]+) of' -eq 0

echo ""
echo "=== INVARIANT TESTS (--gen argon2d) ==="
VERIFIER_EXTRA="--gen argon2d" run_invariant build_honest   9121 correct   $PROVER --port 9121
VERIFIER_EXTRA="--gen argon2d" run_invariant build_fake     9123 all_wrong \
    $CHEAT --port 9123 --mode fake --frac 0.5 --datafile /tmp/memchal_build_fake.bin
VERIFIER_EXTRA="--gen argon2d" run_invariant build_mmapdisk 9124 correct \
    $CHEAT --port 9124 --mode mmapdisk --datafile /tmp/memchal_build_mmap.bin

echo ""
echo "=== INVARIANT TESTS (--chain dependent) ==="
VERIFIER_EXTRA="--chain dependent" run_invariant chain_dep_honest   9201 correct   $PROVER --port 9201
VERIFIER_EXTRA="--chain dependent" run_invariant chain_dep_disk     9202 correct \
    $CHEAT --port 9202 --mode disk --datafile /tmp/memchal_test_dep_disk.bin
VERIFIER_EXTRA="--chain dependent" run_invariant chain_dep_mmapdisk 9203 correct \
    $CHEAT --port 9203 --mode mmapdisk --datafile /tmp/memchal_test_dep_mmap.bin
VERIFIER_EXTRA="--chain dependent" run_invariant chain_dep_prefetch 9204 correct \
    $CHEAT --port 9204 --mode prefetch --datafile /tmp/memchal_test_dep_prefetch.bin
VERIFIER_EXTRA="--chain dependent" run_invariant chain_dep_fake     9205 all_wrong \
    $CHEAT --port 9205 --mode fake --frac 0.5 --datafile /tmp/memchal_test_dep_fake.bin

echo ""
echo "=== INVARIANT TESTS (--fold tree) ==="
VERIFIER_EXTRA="--fold tree" run_invariant tree_honest 9301 correct   $PROVER --port 9301
VERIFIER_EXTRA="--fold tree" run_invariant tree_disk   9302 correct \
    $CHEAT --port 9302 --mode disk --datafile /tmp/memchal_test_tree_disk.bin
VERIFIER_EXTRA="--fold tree" run_invariant tree_fake   9303 all_wrong \
    $CHEAT --port 9303 --mode fake --frac 0.5 --datafile /tmp/memchal_test_tree_fake.bin

echo ""
echo "=== SESSION EVENT STREAM ==="
STREAM_DIR=$(mktemp -d /tmp/memchal_stream_XXXXXX)
$CHEAT --port 9105 --mode mmapdisk --datafile /tmp/memchal_test_stream.bin \
    --trace 10 >"$STREAM_DIR/prover.log" 2>&1 &
STREAM_PID=$!
wait_for_port 9105
build/verifier --port 9105 --mb 8 --samples 512 --lanes 8 --challenges 30 --warmup 10 \
    --csv /dev/null --stream "$STREAM_DIR/verifier.jsonl" --probe-map 10 >/dev/null 2>&1 || true
sleep 0.4
kill "$STREAM_PID" 2>/dev/null || true
wait "$STREAM_PID" 2>/dev/null || true
if python3 tests/session/check_stream.py "$STREAM_DIR/verifier.jsonl" "$STREAM_DIR/prover.log"; then
    pass "session event stream"
else
    fail "session event stream"
fi
rm -rf "$STREAM_DIR"

echo ""
echo "=============================="
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "ALL TESTS PASSED ($PASS_COUNT passed, 0 failed)"
    exit 0
fi
echo "TESTS FAILED: $FAIL_COUNT failed, $PASS_COUNT passed"
for t in "${FAILED_TESTS[@]}"; do echo "  - $t"; done
exit 1
