# Port: example.lu → v5

- **Commit:** uncommitted working tree, base `080f12b` (session 3).
- **Original:** `examples/classic/example.lu` (Classic, moved from `src/`).
- **Port:** `examples/v5/example.lu`.
- **Backends verified:** C and native. Both produce the same stdout as the
  Classic original and exit 0. Output is byte-identical (6 lines).

## Behavior deltas

None at runtime. The Classic original emits one diagnostic that the port does
not:

```
[LU WARN] line 17: Lin reference to '2+2' — no block with matching Pr/ found
```

The reference had no runtime effect in 4.8 (it called no block), so dropping
it preserves behavior and closes T-02 for the shipped example.

## Friction points

1. **Executable top-level blocks.** `#q1`…`#q3` have no v5 form; v5 code lives
   only inside functions (SPEC §7). All statements moved into `fn main`,
   preserving source order. The blocks were sequential, so order is unchanged.
2. **`Def:config timeout = 5000`.** v5 has no `Def:` namespace; it became a
   top-level `vid timeout: int = 5000` (SPEC §3, §12).
3. **Legacy statement spellings.** `Fn/`, `If/`+`To/`+`Else/`, `Loop/N`,
   `Loop/While`, `Set/`, `Ret/`, `Pr/` became `fn`, `if`/`else`, `loop N`,
   `while`, `set`, `ret`, `Pr(...)`. Braces are mandatory in v5.
4. **`Def:anw(2+2)/Lin`.** No v5 equivalent; Classic's block-reuse mechanism is
   replaced by ordinary function calls. Here it was already inert (T-02), so
   the port omits it rather than inventing a call.
5. **File metadata.** `Lu/Language`, `M/ us *user*` dropped; v5 requires
   `Lu/Strict` and has no metadata line.
6. **Header/braces forced a small structural change.** Classic used fall-through
   `If/ … To/ … Else/`; v5 requires block braces. This is the mechanical
   translation described in SPEC §12, not a semantic rewrite.

## Findings filed

- **Doc gap (fixed):** SPEC §12 did not list `#q` blocks, legacy statement
  spellings, `Def:config`, `Lin`, `class`/`enum`, `new`/`Free`, compound
  assignment, `this`, or metadata as Classic deltas. The table now does, and
  names this port as evidence.
- **Doc gap (open, T-09):** GUIDE still presents Classic as the primary
  syntax; the Classic→v5 migration chapter required by charter §9.4 is not
  written. The port report and SPEC §12 are the normative source until then.
- **Spec gap:** none found.
- **Language gap:** none for this program. `Lin` remains Classic-only by
  design; v5 expresses the same intent with functions.
- **T-02:** closed for the shipped example (the port carries no `Lin`
  reference and emits no warning). The frozen Classic diagnostic itself is
  unchanged.

## Section 8 checklist

- Scoping/names: no block-scoped declarations in the original; no shadowing;
  no stdlib name collisions. Pass.
- Types/conversions: only `int`, `bool`, `str`; no narrowing, arrays, pointers
  or literals outside `int`. Pass.
- Control flow: `if`/`else`, `loop 3`, `while`; no `range`, `match`, `break` or
  `continue`. Pass.
- Functions/calls: `check` preserves its signature and returns; the declared
  `bool` result is accepted by the missing-return analysis. Pass.
- Strings/I/O: `Pr` only; no interpolation, files or `Uin`. Pass.
- Data structures: none. Pass.
- `incl`: none. Pass.
- Backend parity: Classic C baseline vs v5 C and v5 native, stdout and exit
  status compared by `tests/ports/run.sh`. Pass.
- Hygiene: no commented-out Classic lines; the only comment explains the
  dropped `Lin` reference.

**Checklist result:** pass, 2026-10-25.
