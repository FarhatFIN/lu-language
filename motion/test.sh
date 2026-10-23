#!/bin/sh
# Deterministic checks for LuMotion.  Run from anywhere:
#
#     motion/test.sh
#
# The renderer is exercised end to end (build, project I/O, PPM output);
# the editor is only built, because it needs an X display to run.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
LUC=${LUC:-$root/src/luc}
CC=${CC:-cc}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() {
    printf '[FAIL] %s\n' "$*" >&2
    exit 1
}

note() {
    printf '[ ok ] %s\n' "$*"
}

# ── build both programs through luc ────────────────────────────────────────
"$LUC" "$here/render.lu" -o "$work/render.c" > "$work/build.log" 2>&1 \
    || { cat "$work/build.log" >&2; fail "luc rejected render.lu"; }
"$CC" -O2 -std=c11 -w "$work/render.c" -lm -o "$work/lumotion-render" \
    || fail "C backend rejected render.c"
note "render.lu builds"

"$LUC" "$here/motion.lu" -o "$work/motion.c" > "$work/build.log" 2>&1 \
    || { cat "$work/build.log" >&2; fail "luc rejected motion.lu"; }
"$CC" -O2 -std=c11 -w "$work/motion.c" -lm -lX11 -o "$work/lumotion" \
    || fail "C backend rejected motion.c (is libX11-dev installed?)"
note "motion.lu builds"

render="$work/lumotion-render"

# ── the demo scene is the reference composition ────────────────────────────
"$render" --demo --info > "$work/demo.txt" || fail "--demo --info failed"
grep -q '^project : lu-motion-title$' "$work/demo.txt" \
    || fail "unexpected project name"
grep -q '^layers  : 7$' "$work/demo.txt" || fail "demo scene lost layers"
grep -q '^size    : 640 x 360 @ 30 fps, 150 frames' "$work/demo.txt" \
    || fail "unexpected composition size"
note "demo composition: 7 layers, 640x360@30, 150 frames"

# ── --still writes a valid binary PPM at the right size ────────────────────
"$render" --demo --still 42 "$work/f42.ppm" > /dev/null || fail "--still failed"
printf 'P6\n640 360\n255\n' > "$work/hdr.expected"
head -c 15 "$work/f42.ppm" | cmp -s - "$work/hdr.expected" \
    || fail "bad PPM header"
size=$(wc -c < "$work/f42.ppm")
expected=$((15 + 640 * 360 * 3))
[ "$size" -eq "$expected" ] || fail "PPM is $size bytes, expected $expected"
note "PPM output: $size bytes, header ok"

# ── rendering is deterministic ─────────────────────────────────────────────
"$render" --demo --still 42 "$work/f42b.ppm" > /dev/null
cmp -s "$work/f42.ppm" "$work/f42b.ppm" || fail "same frame rendered differently"
note "frame 42 is byte-identical across runs"

# ── frame numbers are clamped, not rejected ────────────────────────────────
"$render" --demo --still 9999 "$work/last.ppm" > "$work/clamp.txt"
grep -q '^frame 149 -> ' "$work/clamp.txt" || fail "frame clamp is broken"
note "frame 9999 clamps to 149"

# ── --seq writes the requested range ───────────────────────────────────────
"$render" --demo --seq "$work" 10 12 > /dev/null || fail "--seq failed"
for f in 0010 0011 0012; do
    [ -s "$work/frame_$f.ppm" ] || fail "missing frame_$f.ppm"
done
[ ! -e "$work/frame_0009.ppm" ] || fail "--seq wrote a frame outside the range"
note "--seq 10..12 writes exactly three frames"

# ── project round-trip: save and load agree ────────────────────────────────
"$render" --demo --write "$work/demo.lum" > "$work/write.txt" || fail "--write failed"
grep -q '^wrote .* (7 layers)$' "$work/write.txt" || fail "--write reported wrong count"
"$render" "$work/demo.lum" --info > "$work/loaded.txt" || fail "cannot load written project"
cmp -s "$work/demo.txt" "$work/loaded.txt" || fail "saved project does not round-trip"
note "project save/load round-trips"

# ── the shipped example stays loadable ────────────────────────────────────
"$render" "$here/examples/title.lum" --info > "$work/example.txt" || fail "example project won't load"
grep -q '^layers  : ' "$work/example.txt" || fail "example project has no layers"
note "examples/title.lum loads"

# ── bad input fails loudly ────────────────────────────────────────────────
"$render" "$work/no-such.lum" --info > "$work/missing.txt" || true
grep -q "error: cannot read project" "$work/missing.txt" \
    || fail "missing project produced no diagnostic"
"$render" --demo --bogus > "$work/bogus.txt" || true
grep -q "error: unknown option '--bogus'" "$work/bogus.txt" \
    || fail "unknown option produced no diagnostic"
note "bad input produces diagnostics"

printf '\n[LuMotion] all checks passed\n'
