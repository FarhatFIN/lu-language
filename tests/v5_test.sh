#!/bin/sh
# Lu v5 regression suite.  Run from src via `make test-v5`.
#
# good/*.lu  must build through the native backend and produce exactly the
#            stdout in the matching .stdout file (empty when absent) and the
#            exit status in the matching .rc file (0 when absent).
#
# bad/*.lu   must fail to compile with the diagnostic named in the matching
#            .err file, and must never be reported as successfully built.
set -eu

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
here=$(cd ../tests/v5 && pwd)

checked=0
fail=0

for src in "$here"/good/*.lu; do
    test -f "$src" || continue
    name=$(basename "$src" .lu)
    checked=$((checked + 1))

    if ! ./luc "$src" -o "$work/$name" > "$work/$name.build" 2>&1; then
        printf '[FAIL] %s: v5 program did not compile\n' "$name" >&2
        cat "$work/$name.build" >&2
        fail=$((fail + 1))
        continue
    fi

    set +e
    prog_in=/dev/null
    if [ -f "$here/good/$name.stdin" ]; then
        prog_in="$here/good/$name.stdin"
    fi
    "$work/$name" < "$prog_in" > "$work/$name.stdout" 2>&1
    rc=$?
    set -e

    want_rc=0
    if [ -f "$here/good/$name.rc" ]; then
        want_rc=$(cat "$here/good/$name.rc")
    fi
    if [ "$rc" != "$want_rc" ]; then
        printf '[FAIL] %s: exit status %s, expected %s\n' \
            "$name" "$rc" "$want_rc" >&2
        fail=$((fail + 1))
        continue
    fi

    want_out="$work/$name.want"
    : > "$want_out"
    if [ -f "$here/good/$name.stdout" ]; then
        want_out="$here/good/$name.stdout"
    fi
    if ! diff -u "$want_out" "$work/$name.stdout" > "$work/$name.diff"; then
        printf '[FAIL] %s: unexpected stdout\n' "$name" >&2
        cat "$work/$name.diff" >&2
        fail=$((fail + 1))
        continue
    fi

    # The same program through the C backend must agree, so a fixture pins
    # down both backends rather than only the native one.
    if command -v cc > /dev/null 2>&1; then
        if ! ./luc -c "$src" -o "$work/$name.c" > "$work/$name.cbuild" 2>&1; then
            printf '[FAIL] %s: C backend did not translate\n' "$name" >&2
            cat "$work/$name.cbuild" >&2
            fail=$((fail + 1))
            continue
        fi
        if ! cc -std=c11 -O2 -o "$work/$name.cc" "$work/$name.c" \
                > "$work/$name.cc.err" 2>&1; then
            printf '[FAIL] %s: generated C does not build\n' "$name" >&2
            cat "$work/$name.cc.err" >&2
            fail=$((fail + 1))
            continue
        fi
        set +e
        "$work/$name.cc" < "$prog_in" > "$work/$name.cstdout" 2>&1
        crc=$?
        set -e
        if [ "$crc" != "$want_rc" ]; then
            printf '[FAIL] %s: C backend exit status %s, expected %s\n' \
                "$name" "$crc" "$want_rc" >&2
            fail=$((fail + 1))
            continue
        fi
        if ! diff -u "$want_out" "$work/$name.cstdout" > "$work/$name.cdiff"; then
            printf '[FAIL] %s: stdout differs on the C backend\n' "$name" >&2
            cat "$work/$name.cdiff" >&2
            fail=$((fail + 1))
            continue
        fi
    fi
    printf '[OK]   %s\n' "$name"
done

# `luc <file> make` must run the program and return its exit status.
set +e
make_out=$(./luc "$here/good/05_pack.lu" make 2>&1)
make_rc=$?
set -e
if [ "$make_rc" != 0 ]; then
    printf '[FAIL] make mode: exit status %s\n' "$make_rc" >&2
    printf '%s\n' "$make_out" >&2
    fail=$((fail + 1))
elif ! printf '%s\n' "$make_out" | grep -F 'lit' > /dev/null; then
    printf '[FAIL] make mode: program output missing\n' >&2
    printf '%s\n' "$make_out" >&2
    fail=$((fail + 1))
else
    printf '[OK]   make mode\n'
fi
checked=$((checked + 1))

for src in "$here"/bad/*.lu; do
    test -f "$src" || continue
    name=$(basename "$src" .lu)
    checked=$((checked + 1))

    if ./luc "$src" -o "$work/$name" > "$work/$name.stdout" 2> "$work/$name.err"; then
        printf '[FAIL] %s: v5 program unexpectedly compiled\n' "$name" >&2
        fail=$((fail + 1))
        continue
    fi
    if [ -e "$work/$name" ]; then
        printf '[FAIL] %s: failed compile left a binary behind\n' "$name" >&2
        fail=$((fail + 1))
        continue
    fi
    diagnostic=$(cat "$here/bad/$name.err")
    if ! grep -F "$diagnostic" "$work/$name.err" > /dev/null; then
        printf '[FAIL] %s: missing diagnostic: %s\n' "$name" "$diagnostic" >&2
        cat "$work/$name.err" >&2
        fail=$((fail + 1))
        continue
    fi
    printf '[OK]   %s (rejected)\n' "$name"
done

if [ "$fail" -ne 0 ]; then
    printf '[FAIL] v5: %s of %s checks failed\n' "$fail" "$checked" >&2
    exit 1
fi
printf '[OK]   v5: %s checks passed\n' "$checked"
