# Port: test_full.lu → v5

- **Commit:** uncommitted working tree, base `080f12b` (session 4).
- **Original:** `examples/classic/test_full.lu` (moved from `src/`).
- **Port:** `examples/v5/test_full.lu`.
- **Backends verified:** C and native. stdout byte-identical (3 lines),
  exit 0.

## Behavior deltas

None.

## Friction points

1. **`Def:const MAX = 10`.** v5 has no untyped constant declaration; it
   became `vid MAX: int = 10` (SPEC §3, §12). The value is an `int` literal.
2. **Top-level `#q1`.** Statements moved into `fn main` (SPEC §7).
3. **Legacy spellings.** `If/`+`To/`+`Else/`, `Loop/N`, `Loop/While`, `Set/`,
   `Pr/` became the v5 keyword forms with mandatory braces.
4. **Declaration position.** `Fn/add` was declared after its use in the
   original; the port declares it before `main`. v5 resolves forward
   references in the scan pass, but item order is not semantically
   significant, so this is presentation only.

## Findings filed

- No new gap; T-09 (GUIDE migration chapter) still applies.
- Spec gap: none. Language gap: none.

## Section 8 checklist

Scoping: no inner declarations. Types: `int` only, `MAX` now explicitly
typed. Control flow: `if`, `loop N`, `while`; all match Classic. Functions:
`add` keeps its signature. Strings/I/O: `Pr` only. Data structures and
`incl`: none. Backend parity: compared against the Classic C baseline on both
v5 backends. Hygiene: clean.

**Checklist result:** pass, 2026-10-25.
