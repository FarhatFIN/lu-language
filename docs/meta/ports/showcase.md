# Port: showcase.lu → v5

- **Commit:** uncommitted working tree, base `080f12b` (session 4).
- **Original:** `examples/classic/showcase.lu` (moved from `tests/`).
- **Port:** `examples/v5/showcase.lu`.
- **Backends verified:** C and native. stdout byte-identical to the Classic
  original (8 lines), exit 0.

## Behavior deltas

None. The port keeps the names `fib`, `target`, `value`, `countdown` and the
same statement order.

## Friction points

1. **Top-level `#q1` block.** Statements moved into `fn main` (SPEC §7).
2. **Legacy spellings.** `Fn/`, `If/ … To/`, `Loop/While`, `Set/`, `Pr/`
   became `fn`, `if { }`, `while`, `set`, `Pr(...)`; braces are mandatory.
3. **Metadata.** `Lu/Language`, `Import Russian`, `M/ us *user*` dropped;
   v5 requires `Lu/Strict` and has no metadata or `Import` forms.
4. **Block-scoped `next`.** `int next` inside the `while` body is a fresh v5
   binding per iteration (SPEC §6); no leak, no shadowing conflict. Verified
   by output and by the native/C parity check.

## Findings filed

- No new gap. Same docs debt as the first port: GUIDE needs the Classic→v5
  chapter (T-09); SPEC §12 now documents the forms translated here.
- Spec gap: none.
- Language gap: none.

## Section 8 checklist

Scoping (loop-body `next`): pass. Types (`int` only): pass. Control flow
(`if`, `while`, recursion): pass; recursion depth is trivial. Functions:
explicit annotations preserved, arity checked. Strings/I/O: `Pr` only. Data
structures: none. `incl`: none. Backend parity: C and native vs Classic C,
stdout and exit status compared. Hygiene: no dead Classic lines.

**Checklist result:** pass, 2026-10-25.
