#!/bin/sh
# v5-native capability probes for luget (S0 / Q-02). Prints one line per
# check and exits non-zero if any check deviates from the behavior documented
# in the probe README and docs/meta/phase1-baseline.md §6.
set -u

root=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$root/../../.." && pwd)
luc="$repo/src/luc"

if [ ! -x "$luc" ]; then
    echo "error: $luc not found — run 'make -C src' first" >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

pass=0
fail=0
ok() { printf '[PASS] %s — %s\n' "$1" "$2"; pass=$((pass + 1)); }
no() { printf '[FAIL] %s — %s\n' "$1" "$2"; fail=$((fail + 1)); }

cd "$root" || exit 1

# The probes read data.txt and dirwalk_fixture/ relative to the CWD.
compile_run() { # <src> <out-prefix>
    timeout 20 "$luc" "$1" -o "$2.bin" >"$2.log" 2>&1 || return $?
    timeout 10 "$2.bin" >"$2.stdout" 2>"$2.run.err"
}

expect_reject() { # <name> <src> <out-prefix> <needle>
    timeout 20 "$luc" "$2" -o "$3.bin" >"$3.log" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        no "$1" "compiled successfully, expected a diagnostic"
    elif [ "$rc" -ge 124 ]; then
        no "$1" "timed out (rc=$rc)"
    elif grep -qi -- "$4" "$3.log"; then
        ok "$1" "rejected (rc=$rc) with '$4'"
    else
        no "$1" "rejected without '$4'"; cat "$3.log" >&2
    fi
}

# ── 1. raw file I/O ──
# data.txt is "hello-lu\n"; Pr(&buf[0]) emits the trailing newline, and Pr
# adds its own, hence the blank line.
if compile_run file_io.lu "$work/file_io" &&
   printf 'bytes= 9\nhello-lu\n\n' | diff -u - "$work/file_io.stdout" >/dev/null; then
    ok "file_io" "open/read/close via raw syscalls"
else
    no "file_io" "expected 'bytes= 9' then 'hello-lu'"
    cat "$work/file_io.log" "$work/file_io.stdout" >&2
fi

# ── 2. directory enumeration ──
if compile_run dirwalk.lu "$work/dirwalk"; then
    if grep -qx 'alpha.txt' "$work/dirwalk.stdout" &&
       grep -qx 'beta.lu' "$work/dirwalk.stdout"; then
        ok "dirwalk" "getdents64 decoding finds the fixture entries"
    else
        no "dirwalk" "fixture entries missing from the listing"
        cat "$work/dirwalk.stdout" >&2
    fi
else
    no "dirwalk" "compile failed"; cat "$work/dirwalk.log" >&2
fi

# ── 3. argv is not exposed to v5 ──
expect_reject "argv" argv.lu "$work/argv" "undeclared name '__luc_argc'"

# ── 4. /proc/self/cmdline workaround ──
if compile_run argv_proc.lu "$work/argv_proc" &&
   grep -q "argv_proc.bin" "$work/argv_proc.stdout"; then
    ok "argv_proc" "/proc/self/cmdline yields argv[0]"
else
    no "argv_proc" "expected the probe's own path on stdout"
    cat "$work/argv_proc.log" "$work/argv_proc.stdout" >&2
fi

# ── 5. no argv array, so no spawn ──
expect_reject "spawn" spawn.lu "$work/spawn" "unexpected token '\\['"

# ── 6. no C header import in v5 ──
expect_reject "v5hdr" v5hdr.lu "$work/v5hdr" "include file 'stdlib.h' not found"

# ── 7. native recursion has a hard stack limit ──
if timeout 20 "$luc" recursion.lu -o "$work/rec_deep.bin" >"$work/rec_deep.log" 2>&1; then
    if timeout 20 "$work/rec_deep.bin" >/dev/null 2>&1; then
        no "recursion/deep" "100000 frames completed, expected a stack overflow"
    else
        rc=$?
        if [ "$rc" -eq 124 ]; then
            no "recursion/deep" "timed out instead of crashing"
        else
            ok "recursion/deep" "100000 frames die (rc=$rc, no diagnostic)"
        fi
    fi
else
    no "recursion/deep" "compile failed"; cat "$work/rec_deep.log" >&2
fi

sed 's/^vid DEPTH: int = .*/vid DEPTH: int = 20000/' recursion.lu >"$work/rec_shallow.lu"
if compile_run "$work/rec_shallow.lu" "$work/rec_shallow" &&
   timeout 20 "$work/rec_shallow.bin" >/dev/null 2>&1; then
    ok "recursion/shallow" "20000 frames complete"
else
    no "recursion/shallow" "20000 frames should not overflow"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
