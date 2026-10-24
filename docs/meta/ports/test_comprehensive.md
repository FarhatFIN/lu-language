# Port: test_comprehensive.lu → v5

- **Commit:** uncommitted working tree, base `080f12b` (session 4).
- **Original:** `examples/classic/test_comprehensive.lu` (moved from `src/`).
- **Port:** `examples/v5/test_comprehensive.lu`.
- **Backends verified:** C and native. stdout byte-identical (4 lines),
  exit 0.

## Behavior deltas

None. The unused `Point`, `Color`, and `max` declarations are preserved; the
enum members keep their names and ordinals as immutable globals.

## Friction points

1. **`enum Color`.** v5 has no enums. `RED`, `GREEN`, `BLUE` became top-level
   `vid` constants with values 0, 1, 2 (SPEC §12). They are unused in this
   program, but preserving them keeps the demo's structure.
2. **`Elif/` fall-through.** Became `else if` with braces (SPEC §6).
3. **Null pointer initialisation.** `ptr/int p = 0` has no v5 counterpart:
   there is no null pointer (SPEC §4.4). The port allocates at declaration:
   `var p: ptr/int = Alloc(1)`.
4. **Typed allocation syntax.** `Alloc/int:1` became `Alloc(1)`, with the
   element type taken from `p`'s declared type.
5. **`Memset/p 0 4` / `Free/p`.** Became `Memset(p, 0, 4)` / `Del p`.
6. **Top-level `#q1`.** Statements moved into `fn main` (SPEC §7).

## Findings filed

- **Doc gap:** the Classic “allocate then assign to a null pointer” idiom has
  no v5 form; SPEC §4.4 now states that there is no null pointer. Porters
  must allocate at declaration. T-09 (GUIDE chapter) still applies.
- **Spec gap:** none beyond the §4.4 clarification already filed with the
  previous port.
- **Language gap:** none for this file.

## Section 8 checklist

Scoping: no inner declarations. Types: `int`, `ptr/int`; no conversions.
Control flow: `if`/`else if`/`else`, `loop 4`; parity. Functions: `max`
preserved, explicit return. Strings/I/O: `Pr` only. Data structures: struct
and enum-as-constants declarations, unused. `incl`: none. Backend parity: C
and native against the Classic C baseline. Hygiene: one comment explains the
enum translation; no dead Classic lines.

**Checklist result:** pass, 2026-10-25.
