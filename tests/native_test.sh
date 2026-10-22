#!/bin/sh
# Run from src via `make native-test`.
#
# Every program in tests/native is built twice:
#
#   luc -n  → our own static x86-64 ELF (syscalls only, no libc)
#   luc -c  → C, then cc
#
# The two binaries must agree on stdout and on exit status. When no C
# compiler is installed the native half still runs, so this suite is
# useful on a bare system that cannot use the C backend at all.
set -eu

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

have_cc=1
if ! command -v cc > /dev/null 2>&1; then
    have_cc=0
    printf '[SKIP] no C compiler: comparing native runs only\n'
fi

checked=0
fail=0

for src in ../tests/native/*.lu; do
    test -f "$src" || continue
    name=$(basename "$src" .lu)

    if ! ./luc -n "$src" -o "$work/$name.elf" > /dev/null 2> "$work/$name.err"; then
        printf '[FAIL] %s: native compilation failed\n' "$name" >&2
        cat "$work/$name.err" >&2
        fail=1
        continue
    fi

    if [ ! -x "$work/$name.elf" ]; then
        printf '[FAIL] %s: native output is not executable\n' "$name" >&2
        fail=1
        continue
    fi

    # The whole point of the native backend: a static ELF with no dynamic
    # loader and no shared-library dependencies.
    magic=$(od -An -tx1 -N4 "$work/$name.elf" | tr -d ' \n')
    if [ "$magic" != "7f454c46" ]; then
        printf '[FAIL] %s: native output is not an ELF file\n' "$name" >&2
        fail=1
        continue
    fi
    if command -v readelf > /dev/null 2>&1; then
        if readelf -l "$work/$name.elf" 2>/dev/null | grep -q INTERP ||
           readelf -d "$work/$name.elf" 2>/dev/null | grep -q NEEDED; then
            printf '[FAIL] %s: native ELF depends on a shared library\n' "$name" >&2
            fail=1
            continue
        fi
    fi

    nrc=0
    "$work/$name.elf" > "$work/$name.native.out" 2>&1 || nrc=$?

    if [ "$have_cc" -eq 0 ]; then
        checked=$((checked + 1))
        continue
    fi

    if ! ./luc -c "$src" -o "$work/$name.c" > /dev/null 2> "$work/$name.c.err"; then
        printf '[FAIL] %s: C backend compilation failed\n' "$name" >&2
        cat "$work/$name.c.err" >&2
        fail=1
        continue
    fi

    if ! cc -std=c11 -O2 -o "$work/$name.cc" "$work/$name.c" 2> "$work/$name.cc.err"; then
        printf '[FAIL] %s: generated C does not build\n' "$name" >&2
        cat "$work/$name.cc.err" >&2
        fail=1
        continue
    fi

    crc=0
    "$work/$name.cc" > "$work/$name.c.out" 2>&1 || crc=$?

    if [ "$nrc" -ne "$crc" ]; then
        printf '[FAIL] %s: exit status differs (native %d, C %d)\n' \
            "$name" "$nrc" "$crc" >&2
        fail=1
        continue
    fi

    if ! diff -u "$work/$name.c.out" "$work/$name.native.out" > "$work/$name.diff"; then
        printf '[FAIL] %s: stdout differs between backends\n' "$name" >&2
        cat "$work/$name.diff" >&2
        fail=1
        continue
    fi

    printf '[ok] %s (rc=%d)\n' "$name" "$nrc"
    checked=$((checked + 1))
done

if [ "$checked" -eq 0 ]; then
    printf '[FAIL] no fixtures found in tests/native\n' >&2
    exit 1
fi

[ "$fail" -eq 0 ] || exit 1
printf '[LU NATIVE TEST] %d program(s) matched the C backend\n' "$checked"
