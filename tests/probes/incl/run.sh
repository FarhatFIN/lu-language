#!/bin/sh
# `incl` behavioral probes (S0 / Q-04). Prints one line per check and exits
# non-zero if any check deviates from the behavior documented in
# src/includes.c and the probe README.
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

# compile with the native backend, then run; leaves $out.c.log and $out.stdout
compile_run() { # <src> <out-prefix>
    timeout 20 "$luc" "$1" -o "$2.bin" >"$2.log" 2>&1 || return $?
    timeout 10 "$2.bin" >"$2.stdout" 2>"$2.run.err"
}

expect_stdout() { # <name> <src> <out-prefix> <expected-file>
    if compile_run "$2" "$3"; then
        if diff -u "$4" "$3.stdout" >"$3.diff" 2>&1; then
            ok "$1" "compile+run, stdout matches"
        else
            no "$1" "stdout differs"; cat "$3.diff" >&2
        fi
    else
        no "$1" "compile failed"; cat "$3.log" >&2
    fi
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

# ── 1. resolution order: including file's directory beats the process CWD ──
mkdir -p "$work/ro/cwd/sub"
cp "$root/resolve-order/sub/main.lu" "$root/resolve-order/sub/lib.lu" "$work/ro/cwd/sub/"
printf 'Lu/Language\n\nfn marker() -> str {\n    return "CWD"\n}\n' >"$work/ro/cwd/lib.lu"
printf 'LOCAL\n' >"$work/ro/want-local"
printf 'CWD\n' >"$work/ro/want-cwd"

cd "$work/ro/cwd" || exit 1
if compile_run sub/main.lu "$work/ro/a" && diff -q "$work/ro/want-local" "$work/ro/a.stdout" >/dev/null; then
    ok "resolve-order/relative-first" "sub/lib.lu beats ./lib.lu"
else
    no "resolve-order/relative-first" "expected LOCAL from the including file's directory"
    cat "$work/ro/a.log" "$work/ro/a.stdout" >&2
fi

mv sub/lib.lu sub/lib.lu.off
if compile_run sub/main.lu "$work/ro/b" && diff -q "$work/ro/want-cwd" "$work/ro/b.stdout" >/dev/null; then
    ok "resolve-order/cwd-fallback" "process CWD is searched second"
else
    no "resolve-order/cwd-fallback" "expected CWD fallback to ./lib.lu"
    cat "$work/ro/b.log" "$work/ro/b.stdout" >&2
fi

# ── 2. include-once: diamond ──
mkdir -p "$work/diamond"
cp "$root/once/diamond/"*.lu "$work/diamond/"
printf '42\n6\n' >"$work/diamond.want"
cd "$work/diamond" || exit 1
expect_stdout "once/diamond" main.lu "$work/diamond.r" "$work/diamond.want"

# ── 3. include-once across spellings (<d> vs <./d>) ──
mkdir -p "$work/spelling"
cp "$root/once/spelling/"*.lu "$work/spelling/"
printf '42\n' >"$work/spelling.want"
cd "$work/spelling" || exit 1
expect_stdout "once/spelling" main.lu "$work/spelling.r" "$work/spelling.want"

# ── 3b. the same in v5 (this used to splice the file twice) ──
mkdir -p "$work/spelling-v5"
cp "$root/once/spelling-v5/"*.lu "$work/spelling-v5/"
printf '42\n' >"$work/spelling-v5.want"
cd "$work/spelling-v5" || exit 1
expect_stdout "once/spelling-v5" main5.lu "$work/spelling-v5.r" "$work/spelling-v5.want"

# ── 4. include cycle terminates ──
mkdir -p "$work/cycle"
cp "$root/cycle/"*.lu "$work/cycle/"
printf '3\n' >"$work/cycle.want"
cd "$work/cycle" || exit 1
if timeout 20 "$luc" main.lu -o "$work/cycle.r.bin" >"$work/cycle.r.log" 2>&1; then
    if timeout 10 "$work/cycle.r.bin" >"$work/cycle.r.stdout" 2>&1 &&
       diff -q "$work/cycle.want" "$work/cycle.r.stdout" >/dev/null; then
        ok "cycle/terminates" "a <-> b compiles once and runs"
    else
        no "cycle/terminates" "compiled but output differs"
    fi
else
    rc=$?
    if [ "$rc" -ge 124 ]; then
        no "cycle/terminates" "compiler hung (timeout)"
    else
        no "cycle/terminates" "compiler rejected a cycle (rc=$rc)"; cat "$work/cycle.r.log" >&2
    fi
fi

# ── 5. missing include: diagnostic names the search order ──
mkdir -p "$work/missing"
cp "$root/missing/main.lu" "$work/missing/"
expect_reject "missing/diagnostic" "$work/missing/main.lu" "$work/missing.r" "file not found"

# ── 6. interop: Classic cannot include v5 ──
mkdir -p "$work/interop"
cp "$root/interop/"*.lu "$work/interop/"
cd "$work/interop" || exit 1
expect_reject "interop/classic-includes-v5" classic_main.lu "$work/i1" "Lu/Strict"
expect_reject "interop/v5-includes-classic" v5_main.lu "$work/i2" "first line must be"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
