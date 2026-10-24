# Port: test_core_plus.lu → v5

- **Commit:** uncommitted working tree, base `080f12b` (session 4).
- **Original:** `examples/classic/test_core_plus.lu` (moved from `src/`).
- **Port:** `examples/v5/test_core_plus.lu`.
- **Backends verified:** C and native. stdout byte-identical (5 lines),
  exit 0.

## Behavior deltas

None. `Vec2` remains a value type with a `sum` method; the heap instance
produces the same result.

## Friction points

1. **`class` → `struct`.** v5 has no classes or inheritance. `Vec2` became a
   `struct` with the same fields and method.
2. **Field visibility.** `x` and `y` are read and written from outside the
   struct, so the port marks them `pub`; unmarked fields are private since
   Q5 (SPEC §4.3). This is the first shipped program where the privacy rule
   had to be applied explicitly.
3. **`this` → explicit `self`.** The method takes `fn sum(self) -> int` and
   uses `self.x` (SPEC §4.3, §12).
4. **Array initializer syntax.** `int arr[3] = {1, 2, 3}` became
   `int arr: int[3] = [1, 2, 3]` (SPEC §3, §4.2).
5. **Heap allocation.** `new Vec2()` became `Alloc(1)`; the element type is
   inferred from the declared `ptr/Vec2` (SPEC §4.4). `Free/p` became `Del p`.
   v5 has no null pointer, so allocation is part of the declaration.
6. **Pointer member access.** `p.x` and `p.sum()` work unchanged: v5
   auto-dereferences pointer-to-struct access. This rule was missing from
   SPEC §4.4 and is now documented there.
7. **Top-level `#q1`.** Statements moved into `fn main` (SPEC §7).

## Findings filed

- **Spec gap (fixed):** §4.4 did not state the pointer-to-struct member
  access rule. The port depended on it, so the rule is now normative, with
  this port as evidence.
- **Doc gap:** T-09 (GUIDE migration chapter).
- **Language gap:** none.
- **Compatibility note:** Classic class fields were public; the v5 privacy
  default forces `pub` at the declaration. Expected consequence of Q5, not a
  defect.

## Section 8 checklist

Scoping/names: no collisions; `Vec2`, `add`, `arr`, `v`, `p` preserved.
Types: `int` and `ptr/Vec2`; no implicit conversions. Control flow: straight
line. Functions/methods: arity checked; `sum` returns on all paths.
Data structures: array literal, struct value, heap struct; no out-of-range
access. Strings/I/O: `Pr` only. `incl`: none. Backend parity: C and native
against the Classic C baseline, including the heap path. Hygiene: no comments
and no dead Classic lines.

**Checklist result:** pass, 2026-10-25.
