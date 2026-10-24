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

## 2026-10-25 — LU-ARCH session 3 (S3 hardening: Q5 + Q6, compiler 4.8)

Plan:
- Author said "дальше" after a hardening plan that named Q5 (`pub` enforcement)
  and Q6 (missing-return check) with recommendations; treat it as in-session
  ratification for both. This was S3 semantic hardening — charter §7.7 reserves
  S4 for porting the shipped programs.
- Enforce field privacy in the v5 front-end's emit pass.
- Add structural reachability analysis so non-`void` functions and methods
  cannot fall off the end.
- Make SPEC §4.3/§6 normative, close U-01/U-05, update catalogue and map.
- Bump the compiler version (user-observable rejections), run the full gate.

Done:
- Privacy: `Field.is_pub` is recorded in the scan pass; `check_field_visible`
  rejects non-`pub` access in expression and place position unless the body
  being emitted belongs to the declaring struct (`v->cur_struct`). Methods
  remain public. Diagnostic: `field 'hp' of 'Hero' is private; declare it
  'pub' to use it outside the struct`.
- Returns: `stmt`/`body` now report whether control can fall through;
  `parse_if` requires a final `else` with all branches returning, `parse_match`
  requires `else` with all branches returning, loops are assumed to run zero
  times. Non-`void` functions and methods are rejected with
  `missing return in non-void function 'pick'` /
  `missing return in non-void method 'Box.positive'`. `verr_at` exists so a
  diagnostic at the end of a body names the declaring file and line even when
  the next token belongs to another file.
- `ret` and the result type must agree: bare `ret` in a non-`void` function
  and `ret e` in a `void` function are rejected (SPEC §6 already said so; the
  compiler did not enforce it). Found while probing the new analysis.
- SPEC v0.2: §4.3 and §6 normative, Annex A loses U-01/U-05, §4.1–§12 now say
  `luc 4.8`. `docs/meta/errors.md` gains E-023/E-065/E-066 and promotes
  E-020/E-035/E-050/E-060/E-080/E-010 to stable (tests existed, class did not).
- `tests/conformance/MAP.md`: §4.3, §6, §7 now covered; gaps list drops the
  two closed items.
- Tests: `good/17_flow` (private access from own method, if/else-if/else all
  return, match all return), `bad/29_private_field`, `bad/30_missing_return`,
  `bad/31_missing_return_method`, `bad/32_ret_without_value`,
  `bad/33_ret_value_void`. `test-v5` 33 → 39 checks.
- Version 4.8 (`LUC_VERSION`, README title/footer, CLI banner assertions).

Decisions made: D-0013 (Q5), D-0014 (Q6), D-0015 (version), D-0016
(`ret`/result-type agreement).
Verification: clean build; `test`, `test-all`, `test-cli`, `test-native`,
`test-v5` (39 checks), `agent-test` (66/66), `bootstrap` (identical output),
`tests/probes/incl` (9/9), `tests/probes/luget` (8/8).

Next:
- Q4 (`dv` raw-read semantics) is the only open question.
- S4 (charter §7.7): port the shipped programs starting with `example.lu`;
  T-02 dies with that port. Diagnostics with column (U-07) and the remaining
  conformance gaps ride along.


## 2026-10-25 — LU-ARCH session 4 (S4: first shipped port)

Plan:
- Author said "дальше" after the S3 hardening report.
- Charter §7.7 defines S4 as porting the shipped programs, so start there.
- Move the Classic original to `examples/classic/`, port it to
  `examples/v5/example.lu`, and make the differential check mechanical.

Done:
- Relabelled the Q5/Q6 wave as S3 hardening in `worklog.md`, `SPEC.md` and
  D-0013; "S4" now means the charter's port step.
- `git mv src/example.lu examples/classic/example.lu`; new
  `examples/v5/example.lu` translates `#q1`–`#q3` into `fn main`, `Fn/`/`If/`/
  `Loop/`/`Set/`/`Ret/`/`Pr/` into v5 forms, `Def:config` into a top-level
  `vid`, and drops the metadata line. The inert `Def:anw(2+2)/Lin` reference is
  omitted (T-02 closed for the shipped example).
- Four more corpus programs ported and their originals archived the same way:
  `showcase`, `test_full`, `test_core_plus`, `test_comprehensive`. The ports
  keep names and structure; the only forced changes are v5 forms (`pub`
  fields, explicit `self`, `Alloc`/`Del`, `[1, 2, 3]` array literals, enum
  members as `vid` constants).
- Differential result for all five: Classic C baseline vs v5 C and v5 native —
  stdout byte-identical, exit 0. `test-ports` = 10 checks.
- `tests/ports/backends.txt` (explicit manifest) and `tests/ports/run.sh`;
  `make test-ports`, wired into `test-all`. The runner also rejects any
  `[LU WARN]` from a v5 port build. Negative path verified: a deliberately
  wrong port fails with a diff, then the runner is green again.
- Four corpus files are blocked, not silently skipped: `test_v13` (string
  building, U-11), `test_v20` (v2.0 feature demo: U-02/U-03/U-04, `float`,
  Classic stdlib), `test_paradigms` and `tests/calculator` (`float`).
  `docs/meta/ports/blocked.md` records each construct with the SPEC rule;
  Q7 asks whether v5 should grow.
- SPEC §12 expanded with the Classic deltas the ports exposed (`#q` blocks,
  statement spellings, `Def:`, `Lin`, `class`/`enum`, `new`/`Free`, compound
  assignment, `this`, metadata); §4.4 now states pointer-to-struct
  auto-dereference (`p.field`, `p.method()`), which `test_core_plus` needs.
- Port reports `docs/meta/ports/{example,showcase,test_full,test_core_plus,test_comprehensive}.md`
  (Appendix E.3) with the Section 8 checklist; T-02 closed, T-09 (GUIDE
  migration chapter) and T-10 (blocked corpus) opened in
  `docs/meta/phase1-baseline.md`.
- Makefile `test`/`TEST_PROGRAMS` paths updated; README/GUIDE paths updated;
  GUIDE test table now lists `test-ports` and current check counts.
- Gate found T-11: `bootstrap` listed `lu_compiler.lu` as a prerequisite.
  With a stale `lu_compiler.lu.c` present, GNU make applied its built-in
  `%: %.c` rule and compiled the artifact over the source, destroying it.
  Restored `src/lu_compiler.lu` from HEAD (tracked content unchanged after),
  removed the prerequisite (D-0019), then re-ran `make bootstrap` with a stale
  `.lu.c` present: source intact, `BOOTSTRAP VERIFIED - output is identical`.
  Bootstrap artifacts were removed afterwards.

Decisions made: D-0017 (port layout + manifest), D-0018 (SPEC §12/§4.4
documentation), D-0019 (bootstrap prerequisite).
Verification: `make -B luc` no warnings; `make test`; `make test-all`
(`test-cli` all checks, `test-native` 5/5, `test-v5` 39 checks, `test-ports`
10 checks, all bundled programs compiled and ran); `agent-test` 66/66;
`make bootstrap` verified after the fix; `tests/probes/incl` 9/9;
`tests/probes/luget` 8/8. Runner negative path verified (wrong port fails
with a diff, then green after restore).

Next:
- Await Q7 before touching the blocked corpus.
- S4 order continues: tensor demos → X11 calculator (blocked on `float`, same
  Q7 dependency) → LuMotion.
- U-07 diagnostics (column + `rule:`/`note:`/`hint:`) and the remaining
  conformance gaps are independent of Q7 and can proceed.

## 2026-10-25 — LU-ARCH session 5 (native backend: independence and codegen quality)

Plan:
- Author directive: do not touch LuMotion; develop backend independence, make
  v5 the single backend, make it work without external tooling, reach
  GCC-level code quality. This supersedes charter §7.2's "no performance
  work" non-goal, so it is recorded as ADR D-0020 (A3).
- Keep the native output a freestanding static ELF and make that property a
  test, not a claim.
- Optimize only what the differential suite can verify; every change lands
  with native-vs-C parity green.

Done:
- Static register cache: up to four address-not-taken scalars live in
  r12–r15 for the whole function (`cache_walk` pre-pass, `Var.reg`,
  prologue/epilogue saves). Arrays, `auto` and address-taken locals are
  excluded; synthetic prelude code clears the map so stale entries from a
  previous function cannot leak in.
- Constant division/modulo replaced by round-up magic with a compile-time
  error bound (`signed_magic`: M = floor(2^(64+s)/d)+1, 1 <= e <= 2^(s+1)),
  powers of two by shift; the previous absolute-value sequence was longer
  and lost the sign. Validated offline on 20M random signed cases plus
  structured/range sweeps, and in-tree by 304 native-vs-C cases.
- Branch and operand work: cached-register comparisons, register increments,
  fused compare+branch, direct indexed stores, single-argument call fast
  path, leaf addressing without address temporaries, `acc = acc + expr`
  accumulated straight into the cached register.
- SIB addressing fixed and generalised: `i_lea_rcx_regs` encoded its
  destination as rax instead of rcx (latent, untested); the index may not be
  rsp/r12, but the base may be any register — rsp/r12 need the disp32 form,
  rbp/r13 the disp8 form. Added `tests/native/indexregs.lu`, which exercises
  base r12 and r14 with index r13 and would have caught the old encoding.
- New `i_alu_reg_mem_sib` / `i_alu_reg_mem_disp` fuse the element load into
  `acc ±= p[i]` (64-bit elements). Removed one instruction from the
  remainder path. Loop heads are padded to 16 bytes.
- Argument preparation writes `f(v ± k)` straight into the argument
  register. The 0x110-byte fib frame and call sequence were reviewed; the
  remaining fib gap is gcc's eight-level recursion unrolling, not call
  overhead.
- `tests/native/divmod.lu`: constant divisors 2, 3, 7, 8, 16, 1000, 1024,
  1000003, 2147483647 and their negatives, dividends across 0, ±1, ±7,
  INT64_MAX, INT64_MIN, plus runtime divisors (idiv path).
- `tests/native_test.sh` already rejects a `PT_INTERP` or `DT_NEEDED`;
  re-verified: native binaries are `ELF 64-bit LSB executable, statically
  linked, no section header`, no dynamic section.

Measured (best of five, same host; gcc 15.3.0 reference):
- arith 20M: native 0.0615 s, gcc 0.0618 s — 1.00x.
- mem 5M: native 0.0127 s, gcc 0.0101 s — 1.26x.
- fib(30): native 0.0066 s, gcc 0.0031 s — 2.12x (gcc unrolls recursion).
- Baseline at session start was 0.279 / 0.079 / 0.012 native; the Author's
  untracked `BENCHMARK.md` still shows those numbers. It was left untouched
  because it is the Author's file; the current table is in ADR D-0020.

Decisions made: D-0020 (backend program, native primary, performance in
scope, motion excluded).
Verification: `make -C src test-all` green; `test-native` 7/7 differential
(core, divmod, indexregs, logic, loops, memory, syscalls); `test-v5` 39
checks; `test-ports` 10 checks; `test-cli` all checks; `agent-test` 66/66;
`luc -n` output inspected with `readelf`/`file` (static, no INTERP/NEEDED).

Next:
- Continue codegen: inlining/recursion unrolling is the remaining lever for
  fib-like code; LICM for magic constants and span-based register liveness
  are candidates, each behind the parity suite.
- Nothing committed or pushed; the working tree holds the v4.8 wave plus this
  backend work pending Author approval.
