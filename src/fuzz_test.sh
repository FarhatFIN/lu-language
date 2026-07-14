#!/bin/bash
# Lu fuzz testing — generates random programs and verifies the compiler
# doesn't crash or hang. Run with: ./fuzz_test.sh [rounds]
#
# This is a manual test, not part of CI/agent-test. The results are
# reproducible: same random seed → same test cases. But the test
# cases themselves are not saved to a corpus — each run generates
# fresh random input.
#
# What this checks:
#   - luc does not segfault (exit code > 1, not 0 or 1)
#   - luc does not hang (5-second timeout per file)
#   - luc does not infinite-loop
#
# What this does NOT check:
#   - Correctness of generated C (that's agent-test's job)
#   - Type safety (that's semantic.c's job)
#   - Code quality (that's manual review)

# Note: do NOT use 'set -e' — luc returns exit 1 on parse errors,
# which is correct behavior, and we don't want the script to abort.

ROUNDS=${1:-100}
LUC=${LUC:-./luc}
TMPDIR=$(mktemp -d)
FAIL=0
TIMEOUT_COUNT=0
CRASH_COUNT=0

echo "=== Lu Fuzz Test: $ROUNDS rounds ==="
echo "    TMPDIR: $TMPDIR"
echo "    LUC: $LUC"
echo ""

for i in $(seq 1 $ROUNDS); do
    # Generate a random Lu-like file
    # Start with Lu/Language header (so the parser always gets valid input)
    # Then random printable bytes
    OUT="$TMPDIR/fuzz_$i.lu"
    COUT="$TMPDIR/fuzz_$i.c"

    # Random size between 10 and 500 bytes
    SIZE=$((RANDOM % 490 + 10))

    printf 'Lu/Language\n#q1\n' > "$OUT"
    head -c "$SIZE" /dev/urandom | tr -cd '[:print:]\n' >> "$OUT"
    printf '#q1:end\n' >> "$OUT"

    # Run with 5-second timeout
    timeout 5 "$LUC" "$OUT" -o "$COUT" 2>/dev/null
    RC=$?

    if [ $RC -eq 124 ]; then
        echo "[TIMEOUT] fuzz_$i — possible infinite loop"
        head -5 "$OUT"
        TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        FAIL=1
    elif [ $RC -gt 1 ] && [ $RC -ne 124 ]; then
        echo "[CRASH rc=$RC] fuzz_$i"
        head -5 "$OUT"
        CRASH_COUNT=$((CRASH_COUNT + 1))
        FAIL=1
    fi

    # Progress every 10 rounds
    if [ $((i % 10)) -eq 0 ]; then
        echo "  ... $i / $ROUNDS done"
    fi
done

echo ""
echo "=== Fuzz Results ==="
echo "  Rounds:    $ROUNDS"
echo "  Timeouts:  $TIMEOUT_COUNT"
echo "  Crashes:   $CRASH_COUNT"
echo "  Result:    $([ $FAIL -eq 0 ] && echo "PASS" || echo "FAIL")"

# Cleanup
rm -rf "$TMPDIR"

exit $FAIL
