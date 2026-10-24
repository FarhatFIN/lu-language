#!/bin/sh
# Run from src via make test-cli; keep fixtures outside the source tree.
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf 'print(42)\n' > "$work/source file.lu"

expect_failure() {
    diagnostic=$1
    shift
    if "$@" > "$work/stdout" 2> "$work/stderr"; then
        printf '[FAIL] command unexpectedly succeeded: %s\n' "$*" >&2
        exit 1
    fi
    if ! grep -F "$diagnostic" "$work/stderr" > /dev/null; then
        cat "$work/stderr" >&2
        printf '[FAIL] missing diagnostic: %s\n' "$diagnostic" >&2
        exit 1
    fi
    if grep -F '[luc] wrote ' "$work/stderr" > /dev/null; then
        printf '[FAIL] failed command reported successful output\n' >&2
        exit 1
    fi
}

for level in 0 1 2 3; do
    ./luc "-O$level" "$work/source file.lu" -o "$work/output.c" 2> "$work/stderr"
    test -s "$work/output.c"
    grep -F "Lu Classic, O$level)" "$work/stderr" > /dev/null
done

# Banner/--version: compiler version plus both syntax generations (charter 6.1).
test "$(./luc -v)" = "luc 4.7 (Lu v5 canonical; Lu Classic frozen)"
./luc -h 2>&1 | head -1 | grep -F "Lu Compiler  v4.7 (Lu v5 canonical; Lu Classic frozen)" > /dev/null
printf 'Lu/Strict\nfn main() -> int {\n    ret 0\n}\n' > "$work/v5.lu"
./luc "$work/v5.lu" -o "$work/v5prog" 2> "$work/stderr"
grep -F "compiling '$work/v5.lu'  (Lu v5, O2)" "$work/stderr" > /dev/null
# No -o: default backend is native, so this must produce a runnable ELF.
./luc "$work/source file.lu" -o "$work/prog" 2> "$work/stderr"
test -x "$work/prog"
test "$("$work/prog")" = 42

# -c forces the C backend, which then writes <input>.c next to the input.
./luc -c "$work/source file.lu" 2> "$work/stderr"
test -s "$work/source file.c"

cat > "$work/tensor.lu" <<'EOF'
Lu/Language
#include <tensor.h>
def main() -> void {
    Tensor a = tensor_eye(2)
    Tensor b = tensor_scale(a, 2)
    print(tensor_sum(b))
    tensor_free(b)
    tensor_free(a)
}
EOF
./luc "$work/tensor.lu" -o "$work/tensor.c" 2> "$work/stderr"
grep -F '#include <tensor.h>' "$work/tensor.c" > /dev/null
if grep -F 'typedef struct lu_tensor_storage' "$work/tensor.c" > /dev/null; then
    printf '[FAIL] tensor implementation leaked into generated C\n' >&2
    exit 1
fi
cc -std=c11 -Wall -Wextra -Wpedantic -I../include "$work/tensor.c" -lm -o "$work/tensor"
test "$("$work/tensor")" = 4

cat > "$work/tensor-no-include.lu" <<'EOF'
Lu/Language
def main() -> void { Tensor a = tensor_zeros(2, 2) }
EOF
expect_failure 'tensor support requires #include <tensor.h>' \
    ./luc "$work/tensor-no-include.lu" -o "$work/rejected.c"

cat > "$work/tensor-bad-arity.lu" <<'EOF'
Lu/Language
#include <tensor.h>
def main() -> void { Tensor a = tensor_zeros(2) }
EOF
expect_failure "'tensor_zeros' expects 2 argument(s), got 1" \
    ./luc "$work/tensor-bad-arity.lu" -o "$work/rejected.c"

cat > "$work/tensor-bad-type.lu" <<'EOF'
Lu/Language
#include <tensor.h>
def main() -> void { Tensor a = tensor_scale(1, 2) }
EOF
expect_failure "'tensor_scale' argument 1 expects Tensor, got int" \
    ./luc "$work/tensor-bad-type.lu" -o "$work/rejected.c"

cat > "$work/tensor-wrong-import.lu" <<'EOF'
Lu/Language
Import "<tensor.h>"
def main() -> void { Tensor a = tensor_zeros(2, 2) }
EOF
expect_failure 'tensor library must be enabled exactly with #include <tensor.h>' \
    ./luc "$work/tensor-wrong-import.lu" -o "$work/rejected.c"

cat > "$work/tensor-bounds.lu" <<'EOF'
Lu/Language
#include <tensor.h>
def main() -> void {
    Tensor a = tensor_zeros(1, 1)
    print(tensor_get(a, 1, 0))
}
EOF
./luc "$work/tensor-bounds.lu" -o "$work/tensor-bounds.c" 2> "$work/stderr"
cc -std=c11 -I../include "$work/tensor-bounds.c" -lm -o "$work/tensor-bounds"
if "$work/tensor-bounds" > "$work/stdout" 2> "$work/stderr"; then
    printf '[FAIL] out-of-bounds tensor access unexpectedly succeeded\n' >&2
    exit 1
fi
grep -F 'tensor: index out of bounds' "$work/stderr" > /dev/null

# ── v5 reaches tensor.h: `incl <tensor.h>` is a real C include ──
cat > "$work/tensor-v5.lu" <<'EOF'
Lu/Strict

incl <tensor.h>

fn main() -> int {
    var a: Tensor = tensor_full(2, 2, 3)
    var e: Tensor = tensor_eye(2)
    var s: Tensor = tensor_add(a, e)
    tensor_print(s)
    var n: int = tensor_rows(s)
    tensor_free(s)
    tensor_free(e)
    tensor_free(a)
    ret n
}
EOF
./luc "$work/tensor-v5.lu" -c -o "$work/tensor-v5.c" 2> "$work/stderr"
grep -F '#include <tensor.h>' "$work/tensor-v5.c" > /dev/null
if grep -F 'typedef struct lu_tensor_storage' "$work/tensor-v5.c" > /dev/null; then
    printf '[FAIL] tensor implementation leaked into v5 output\n' >&2
    exit 1
fi
cc -std=c11 -I../include "$work/tensor-v5.c" -lm -o "$work/tensor-v5"
printf 'Tensor[2,2]\n  [4 3]\n  [3 4]\n' > "$work/tensor-v5.expected"
set +e
"$work/tensor-v5" > "$work/stdout"
rc=$?
set -e
test "$rc" -eq 2
cmp -s "$work/stdout" "$work/tensor-v5.expected"

cat > "$work/tensor-v5-no-include.lu" <<'EOF'
Lu/Strict

fn main() -> int {
    var a: Tensor = tensor_zeros(2, 2)
    tensor_free(a)
    ret 0
}
EOF
expect_failure "type 'Tensor' needs \`incl <tensor.h>\`" \
    ./luc "$work/tensor-v5-no-include.lu" -c -o "$work/rejected.c"

cat > "$work/tensor-v5-bad-arity.lu" <<'EOF'
Lu/Strict

incl <tensor.h>

fn main() -> int {
    var a: Tensor = tensor_zeros(2)
    tensor_free(a)
    ret 0
}
EOF
expect_failure "'tensor_zeros' expects 2 argument(s), got 1" \
    ./luc "$work/tensor-v5-bad-arity.lu" -c -o "$work/rejected.c"

expect_failure 'tensor.h needs the C backend' ./luc "$work/tensor-v5.lu" -o "$work/tensor-v5"

# ── v4 `incl`: source splice, include-once, C header passthrough ──
mkdir -p "$work/lib"
cat > "$work/lib/util.lu" <<'EOF'
Lu/Language

def twice(int x) -> int {
    return x * 2
}
EOF
cat > "$work/app.lu" <<'EOF'
Lu/Language
incl <lib/util.lu>
incl <./lib/util.lu>
incl <lib/../lib/util.lu>
incl <stdio.h>
def main() -> void {
    print(twice(21))
}
EOF
./luc "$work/app.lu" -o "$work/app.c" 2> "$work/stderr"
grep -F '#include <stdio.h>' "$work/app.c" > /dev/null
cc -std=c11 "$work/app.c" -o "$work/app"
test "$("$work/app")" = 42

# A cycle must terminate, and each file must still be spliced only once.
cat > "$work/lib/a.lu" <<'EOF'
Lu/Language
incl <b.lu>
def fa() -> void { print("a") }
EOF
cat > "$work/lib/b.lu" <<'EOF'
Lu/Language
incl <a.lu>
def fb() -> void { print("b") }
EOF
cat > "$work/cycle.lu" <<'EOF'
Lu/Language
incl <lib/a.lu>
def main() -> void {
    fa()
    fb()
}
EOF
./luc "$work/cycle.lu" -o "$work/cycle.c" 2> "$work/stderr"
cc -std=c11 "$work/cycle.c" -o "$work/cycle"
test "$("$work/cycle" | tr '\n' ' ')" = "a b "

cat > "$work/missing-include.lu" <<'EOF'
Lu/Language
incl <nope.lu>
def main() -> void { }
EOF
expect_failure 'incl <nope.lu> — file not found' \
    ./luc "$work/missing-include.lu" -o "$work/rejected.c"

cat > "$work/lib/strict.lu" <<'EOF'
Lu/Strict

fn helper() -> int { ret 1 }
EOF
cat > "$work/includes-strict.lu" <<'EOF'
Lu/Language
incl <lib/strict.lu>
def main() -> void { }
EOF
expect_failure 'v4 units can only include Lu/Language files' \
    ./luc "$work/includes-strict.lu" -o "$work/rejected.c"

cat > "$work/tensor-shape.lu" <<'EOF'
Lu/Language
#include <tensor.h>
def main() -> void {
    Tensor a = tensor_zeros(2, 2)
    Tensor b = tensor_zeros(2, 3)
    tensor_print(tensor_add(a, b))
}
EOF
./luc "$work/tensor-shape.lu" -c -o "$work/tensor-shape.c" 2> "$work/stderr"
cc -std=c11 -I../include "$work/tensor-shape.c" -lm -o "$work/tensor-shape"
if "$work/tensor-shape" > "$work/stdout" 2> "$work/stderr"; then
    printf '[FAIL] mismatched tensor shapes unexpectedly succeeded\n' >&2
    exit 1
fi
grep -F 'tensor: elementwise: shape mismatch [2,2] vs [2,3]' "$work/stderr" > /dev/null

cat > "$work/removed-network.lu" <<'EOF'
Lu/Language
#q1
Snd{"payload"}
#q1:end
EOF
expect_failure 'network stubs were removed' \
    ./luc "$work/removed-network.lu" -o "$work/rejected.c"

for level in bogus 2junk 4 -1 999999999999999999999999 ''; do
    expect_failure 'invalid optimisation level' ./luc -O "$level" "$work/source file.lu"
done
expect_failure 'expected exactly one source file' ./luc "$work/source file.lu" "$work/missing.lu"
expect_failure 'Usage:' ./luc
expect_failure "$work/missing.lu" ./luc "$work/missing.lu"
expect_failure "$work/missing/output.c" ./luc "$work/source file.lu" -o "$work/missing/output.c"

# ── make mode: compile natively, run it, return the program's exit status ──
mkdir -p "$work/tmp"
cat > "$work/run-me.lu" <<'EOF'
Lu/Language
fn main() -> int {
    print("made it")
    return 7
}
EOF
set +e
TMPDIR="$work/tmp" ./luc "$work/run-me.lu" make > "$work/stdout" 2> "$work/stderr"
rc=$?
set -e
test "$rc" -eq 7
test "$(cat "$work/stdout")" = "made it"
if find "$work/tmp" -name 'lu-make-*' | grep . > /dev/null; then
    printf '[FAIL] make mode left a temporary binary behind\n' >&2
    exit 1
fi

# With -o the binary is kept on disk and still runs.
set +e
TMPDIR="$work/tmp" ./luc -o "$work/kept" "$work/run-me.lu" make > /dev/null 2>&1
rc=$?
set -e
test "$rc" -eq 7
test -x "$work/kept"

expect_failure 'cannot be combined with -c' ./luc -c "$work/source file.lu" make
expect_failure 'writes an executable, not C source' ./luc -o "$work/out.c" "$work/source file.lu" make

if [ -c /dev/full ]; then
    expect_failure 'failed to write output' ./luc "$work/source file.lu" -o /dev/full
else
    printf '[SKIP] /dev/full unavailable; output write failure test skipped\n'
fi
printf '[LU CLI TEST] all checks passed\n'
