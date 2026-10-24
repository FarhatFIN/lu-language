#!/bin/sh
# Differential verification for the S4 ports (charter §7.7): every v5 port
# must reproduce the Classic original's stdout and exit status on each
# backend listed in backends.txt, and must compile without Classic warnings.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
luc="$root/src/luc"
manifest="$root/tests/ports/backends.txt"
cc=${CC:-cc}

[ -x "$luc" ] || { echo "test-ports: $luc not built" >&2; exit 1; }
[ -f "$manifest" ] || { echo "test-ports: $manifest missing" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

fail=0
checks=0

while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    # shellcheck disable=SC2086  # the manifest is word-split on purpose
    set -- $line
    name=$1
    shift
    [ $# -gt 0 ] || { echo "test-ports: no backend listed for $name" >&2; fail=1; continue; }

    classic="$root/examples/classic/$name"
    port="$root/examples/v5/$name"
    [ -f "$classic" ] || { echo "test-ports: missing Classic $classic" >&2; fail=1; continue; }
    [ -f "$port" ] || { echo "test-ports: missing v5 port $port" >&2; fail=1; continue; }

    base=${name%.lu}
    classic_exe="$work/$base.classic"

    if ! "$luc" -c "$classic" -o "$work/$base.classic.c" \
            >"$work/$base.classic.build.out" 2>"$work/$base.classic.build.err"; then
        echo "test-ports: $name: Classic original failed to compile" >&2
        cat "$work/$base.classic.build.err" >&2
        fail=1
        continue
    fi
    if ! "$cc" -O2 -std=c11 -I"$root/src" -I"$root/include" \
            -o "$classic_exe" "$work/$base.classic.c" -lm \
            >"$work/$base.classic.cc.out" 2>"$work/$base.classic.cc.err"; then
        echo "test-ports: $name: Classic generated C failed to build" >&2
        cat "$work/$base.classic.cc.err" >&2
        fail=1
        continue
    fi

    set +e
    "$classic_exe" >"$work/$base.classic.stdout" 2>"$work/$base.classic.stderr"
    classic_rc=$?
    set -e

    for backend in "$@"; do
        exe="$work/$base.$backend"
        build_err="$work/$base.$backend.build.err"
        case "$backend" in
            c)
                if ! "$luc" -c "$port" -o "$work/$base.port.c" \
                        >"$work/$base.c.build.out" 2>"$build_err"; then
                    echo "test-ports: $name: v5 port failed on the C backend" >&2
                    cat "$build_err" >&2
                    fail=1
                    continue
                fi
                if ! "$cc" -O2 -std=c11 -I"$root/src" -I"$root/include" \
                        -o "$exe" "$work/$base.port.c" -lm \
                        >"$work/$base.c.cc.out" 2>"$work/$base.c.cc.err"; then
                    echo "test-ports: $name: v5 generated C failed to build" >&2
                    cat "$work/$base.c.cc.err" >&2
                    fail=1
                    continue
                fi
                ;;
            native)
                if ! "$luc" -n "$port" -o "$exe" \
                        >"$work/$base.native.build.out" 2>"$build_err"; then
                    echo "test-ports: $name: v5 port failed on the native backend" >&2
                    cat "$build_err" >&2
                    fail=1
                    continue
                fi
                ;;
            *)
                echo "test-ports: $name: unknown backend '$backend'" >&2
                fail=1
                continue
                ;;
        esac

        if grep -q '\[LU WARN\]' "$build_err"; then
            echo "test-ports: $name: v5 port emitted a Classic warning on $backend" >&2
            cat "$build_err" >&2
            fail=1
            continue
        fi

        set +e
        "$exe" >"$work/$base.$backend.stdout" 2>"$work/$base.$backend.stderr"
        rc=$?
        set -e

        if [ "$rc" -ne "$classic_rc" ]; then
            echo "test-ports: $name ($backend): exit $rc, Classic exit $classic_rc" >&2
            fail=1
            continue
        fi
        if ! cmp -s "$work/$base.classic.stdout" "$work/$base.$backend.stdout"; then
            echo "test-ports: $name ($backend): stdout differs from Classic" >&2
            diff -u "$work/$base.classic.stdout" "$work/$base.$backend.stdout" >&2 || true
            fail=1
            continue
        fi

        checks=$((checks + 1))
        echo "[PORT OK] $name ($backend)"
    done
done < "$manifest"

if [ "$fail" -ne 0 ]; then
    echo "ports: FAILED" >&2
    exit 1
fi
[ "$checks" -gt 0 ] || { echo "test-ports: manifest has no ports" >&2; exit 1; }
echo "ports: $checks checks passed"
