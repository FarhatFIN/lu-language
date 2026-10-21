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
    grep -F "(O$level)" "$work/stderr" > /dev/null
done
./luc "$work/source file.lu" 2> "$work/stderr"
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

if [ -c /dev/full ]; then
    expect_failure 'failed to write output' ./luc "$work/source file.lu" -o /dev/full
else
    printf '[SKIP] /dev/full unavailable; output write failure test skipped\n'
fi
printf '[LU CLI TEST] all checks passed\n'
