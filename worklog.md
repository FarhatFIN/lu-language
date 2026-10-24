# Lu worklog

Append-only. Newest entries at the bottom. Format: charter §14.2.

## 2026-10-25 — LU-ARCH session 1 (baseline audit, S0)

Plan:
- Clean build; record compiler hash and toolchain.
- Run every existing suite and the agent doctor; capture results verbatim.
- Inventory Classic vs v5 usage per file.
- Probe `incl` (Q-04) and archive the results as reproducible probes.
- Probe self-hosting (Q-01) and the v5 native subset needed by luget (Q-02).
- Write `docs/meta/phase1-baseline.md`; create the baseline tag.

Done:
- Clean build from `make -C src clean && make`: ok. Compiler hash at HEAD
  `4d20343`: `31766d3f…c69cb5de`; with the two v5 fixes below:
  `15185df8…6c8432e6` (gcc 15.3.0, C11).
- Suites: `make test` ok; `test-cli` ok; `test-native` 5/5 parity; `test-v5`
  24/24 (22 pre-existing + 2 regressions added); `agent-test` 66/66;
  `test-all` 17/17 bundled programs; `test-gui` compiles+links against X11;
  `test-motion` 10/10.
- Floor caveat: `make test-all` globs `src/*.lu`, so the Author's untracked
  scratch files `src/gh.lu` and `src/minitest.lu` (both invalid v5) abort the
  run. Set aside, the corpus is green; restored afterwards untouched.
- `incl` probes: 9/9 pass (`tests/probes/incl/run.sh`).
- luget capability probes: 8/8 pass (`tests/probes/luget/run.sh`); commands,
  expectations and measured numbers are in `tests/probes/luget/README.md`.
- Q-01: bootstrap passes but is **not** real self-hosting — `lu_compiler.lu`
  is a 95-line shim that delegates lexing and codegen to C helpers
  (`lu_self_lex_cast`, `lu_self_codegen_cast`). The real compiler is C.
- Q-02: file I/O and `getdents64` directory enumeration work on the native
  backend through raw `syscall`; recursion crashes (~25–30k frames, segfault,
  no diagnostic); argv is unavailable in v5; process spawn with argv is not
  expressible; v5 has no FFI (no `Import`), so shell-out to `git` is blocked.
- Fixed two v5 `incl` defects found by probing:
  1. use-after-free: spliced tokens kept pointers into a freed include path,
     corrupting diagnostics (`src/v5.c`, `own_path`/`free_include_state`).
  2. include-once divergence: v5 compared raw paths, so `<d5>` vs `<./d5>`
     spliced the same file twice; Classic used `realpath`. v5 now canonicalises
     for the dedupe key (`canonical_path`).
  Regressions: `tests/v5/good/14_include_diamond.lu`,
  `tests/v5/bad/21_included_file_error.lu`.

Decisions made: D-0001, D-0002 (A2 fixes), D-0003 (probes as A1),
D-0004 (annotated tag `v4.6-baseline` created at `4d20343`, local only).
Decisions needed: see `docs/meta/decisions.md` — baseline tag push, commit
policy, `test-all` scratch-file policy, next step choice (S1 vs S3).
Assumptions: A-01…A-03 in `docs/meta/assumptions.md`.

Next:
- Author ratifies tag push + commit.
- Then S1 (naming/identity) or S3 (SPEC core); `test-all` glob policy must be
  settled first so the floor is green without manual file juggling.

## 2026-10-25 — LU-ARCH session 2 (Q1/Q2/Q3 executed: S1 + S3 core)

Plan:
- Ratified by the Author with "сделай всё": commit + push, explicit
  `test-all` list, S3 SPEC core with S1 riding along.
- S1: banner/version/`--version`, per-compilation dialect, naming sweep,
  README "Which syntax?".
- S3: `SPEC.md` v0.1, `docs/meta/errors.md` v1, conformance map, tests for
  every normative MUST that had none.
- Verify the release gate, commit, tag `v4.7`, push branch and tags.

Done:
- S1: `LUC_VERSION` 4.7; `-v` → `luc 4.7 (Lu v5 canonical; Lu Classic
  frozen)`; compile line reports `Lu v5`/`Lu Classic`; CLI test asserts both.
  "Lu/Strict v5" labels swept from README/GUIDE/SYNTAX/TUTORIAL/PATCH_NOTES
  (the literal header token stays). README gained "Какой синтаксис выбрать".
  Fixed stale claims: self-hosting removed (Q-01), FFI marked Classic-only.
- S2 delivered opportunistically: `docs/meta/classic-freeze.md`,
  `CONTRIBUTING.md` (§6.3 criteria verbatim).
- S3: `SPEC.md` v0.1 (1–12 + Annex A/B), `docs/meta/errors.md` (38 IDs,
  stable/provisional classes), `tests/conformance/MAP.md`.
- Probes behind the spec: `dv(&x)` is a raw `read`, not parsing (U-06; SYNTAX
  fixed); `pub` is not enforced (U-05); missing-return not checked (U-01);
  no `auto`/`Vector`/`f"..."`; `return` rejected in v5; string `==`/`match`
  compare content.
- New tests: good/15_match, good/16_loops; bad/22–28. `test-v5` 22 → 33.
- Fixed v5 range-for scoping: two `for i in 0..n` loops emitted `int i`
  twice in C. Lowering now opens a synthetic scope (D-0010).
- Found T-08: bare Classic `{ }` block executes in reverse order on native
  and generates invalid C. Classic is frozen; v5 avoids bare blocks.

Decisions made: D-0005…D-0012. Decisions needed: Q4 (`dv` semantics),
Q5 (`pub` enforcement), Q6 (missing-return check).
Verification: `make -C src test-all`, `agent-test`, `bootstrap.sh`, both probe
runners — results recorded below.

Next:
- S4 (port corpus to v5) or close Q4–Q6 first; both are A3 when they change
  language semantics.
