# Decision ledger

Running table (charter §14.3). A decision not written here (or in an ADR)
did not happen.

| ID | Date | Level | Decision | Rationale | Dissent / review trigger |
|---|---|---|---|---|---|
| D-0001 | 2026-10-25 | A2 | Keep include-path strings alive for the whole v5 transpile (`own_path`, `free_include_state` in `src/v5.c`). | Spliced tokens store `const char *file`; the path was freed right after splicing, so any later diagnostic printed heap garbage. Crash-class defect; regression `tests/v5/bad/21_included_file_error`. | none |
| D-0002 | 2026-10-25 | A2 | Canonicalise the include path for the v5 include-once key (`canonical_path` = `realpath`), while diagnostics keep the resolved spelling. | Classic already did this; v5 spliced `<d5>` and `<./d5>` twice, producing "redeclaration of function" on a valid diamond. Regression `tests/v5/good/14_include_diamond`. | If SPEC §8 later blesses spelling-sensitive inclusion, revisit. |
| D-0003 | 2026-10-25 | A1 | `incl` probes live in `tests/probes/incl/` with a runner and README. | Q-04 needs evidence, not folklore; probes graduate into conformance tests once SPEC §8 exists. | none |
| D-0004 | 2026-10-25 | A2 | Annotated tag `v4.6-baseline` created at `4d20343`. | Charter §5.3 requires the tag; R-1 requires a recovery anchor. | Pushed with Author approval in session 2. |
| D-0005 | 2026-10-25 | A3 | **Q1 ratified:** commit session work and push branch + tags. | Author answered the decision batch with "сделай всё" — explicit in-session approval for the recommended option (charter A4). | Remote history changes are forbidden (R-4). |
| D-0006 | 2026-10-25 | A2 | **Q2 ratified:** `test-all` iterates an explicit `TEST_PROGRAMS` list, not `src/*.lu`. | A scratch file in `src/` could break the floor (T-01); the v5 suite owns its own discovery. | Revisit if shipped program count outgrows the list. |
| D-0007 | 2026-10-25 | A3 | **Q3 ratified:** S3 (SPEC core) first, S1 along with it. | Author approved the recommendation; SPEC is the long pole. | S1 finished in the same session. |
| D-0008 | 2026-10-25 | A2 | `luc` 4.7 banner/`--version`: `luc 4.7 (Lu v5 canonical; Lu Classic frozen)`; each compilation reports `Lu v5` or `Lu Classic`. | Charter §6.1/§6.2; the CLI change is user-observable, so the compiler version increments. Test: `tests/cli_test.sh`. | none |
| D-0009 | 2026-10-25 | A2 | User-facing "Lu/Strict v5" labels renamed to "Lu v5" / "Lu Classic" in README, GUIDE, SYNTAX, TUTORIAL, PATCH_NOTES, test comments. The literal header token `Lu/Strict` is unchanged. | Charter §6.1 sweep. The token names the file format, not the language. | ADRs/freeze ledger keep historical names. |
| D-0010 | 2026-10-25 | A2 | v5 range-for lowering wraps its synthesised `__lu_stN` / loop variable / `__lu_enN` in a synthetic block (`open_scope`/`close_scope` in `src/v5.c`). | Two `for i in 0..n` loops in one function emitted `int i` twice at C function scope: the C backend failed with "redefinition of 'i'". Regression `tests/v5/good/16_loops.lu`. | The synthetic block prints as `if true { … }` because a bare v4 block executes in reverse order on the native backend (new finding, T-08). |
| D-0011 | 2026-10-25 | A1 | S2 delivered opportunistically: `docs/meta/classic-freeze.md` and `CONTRIBUTING.md` with the §6.3 exception criteria. | README §"Which syntax?" must link a freeze ledger; the policy had to be visible in-repo. CI labelling guard (S2.3) remains open. | none |
| D-0012 | 2026-10-25 | A1 | S3 core: `SPEC.md` v0.1 (sections 1–12 + Annex A/B), `docs/meta/errors.md` v1, `tests/conformance/MAP.md`, and 9 new v5 tests so the normative MUSTs are test-backed. | Charter §7.6 steps 1–4; D2 requires spec text and test together. | Unimplemented claims are recorded in Annex A, not silently normative. |
| D-0013 | 2026-10-25 | A3 | **Q5 ratified:** enforce `pub`. Reading or writing a private field outside the declaring struct's methods is now a compile error (`field '%s' of '%s' is private…`). | Author answered the S3 hardening plan with "дальше", which named Q5 with recommendation (a). SPEC §4.3 claimed "private by default" while the compiler allowed everything; the claim and the compiler now agree. Rejections are a source-compatibility break, hence A3. | If a shipped v5 program depended on private access, it must add `pub` or a method; none in-tree did. |
| D-0014 | 2026-10-25 | A3 | **Q6 ratified:** structural missing-return analysis for non-`void` functions and methods. Blocks, `if`/`else if`/`else` and `match` with `else` are analysed; loops are assumed to run zero times. | Same authorization (Q6, recommendation (a)). A falling-off end returned an undefined value on the native backend, which is a correctness hole, not a style issue. Conservative by design: a `ret` inside a loop does not satisfy the check. | Forms rejected only because the analysis is conservative (e.g. `while true { ret 1 }`) are a known limitation; revisit if they appear in real code. |
| D-0015 | 2026-10-25 | A2 | Compiler version 4.8. | D-0008: user-observable changes increment the version. The new rejection classes (privacy, return coverage, `ret`/result-type agreement) and their diagnostics qualify. | none |
| D-0016 | 2026-10-25 | A2 | `ret` must agree with the result type: bare `ret` in a non-`void` function and `ret e` in a `void` function are rejected. | SPEC §6 already stated this ("allowed only in `void` functions"); the compiler did not enforce it, so the text was false. Enforcing the written rule, not adding a new one. | none |
| D-0017 | 2026-10-25 | A2 | S4 port layout: Classic originals live in `examples/classic/`, v5 ports in `examples/v5/`; `tests/ports/backends.txt` declares which backends each port must match, and `make test-ports` runs the differential check (wired into `test-all`). | Charter §7.7 requires the originals to stay as the differential baseline and every port to be verified per backend. An explicit manifest keeps backend coverage visible instead of inferred from code. `example.lu` is the first entry. | If a port cannot run on the native backend, list only `c` and record why in its port report. |
| D-0018 | 2026-10-25 | A2 | SPEC §12 gained rows for `#q` blocks, legacy statement spellings, `Def:config`/`Def:const`, `Lin`, `class`/`enum`, `new`/`Free`, compound assignment, implicit `this`, and metadata. | The first port had to translate all of these, but the normative delta table did not mention them; a porter reading §12 would have been surprised. This documents observed behavior; it does not change the language. | None; evidence is `examples/v5/example.lu` through `make test-ports`. |
| D-0019 | 2026-10-25 | A2 | `make bootstrap` no longer lists `lu_compiler.lu` as a prerequisite; the file is an input consumed by `bootstrap.sh`. | With a stale `lu_compiler.lu.c` present, GNU make's built-in `%: %.c` rule treated `lu_compiler.lu` as a build target and compiled the artifact over the source, destroying it. The source was restored from HEAD and the fixed-point run re-verified. | If `lu_compiler.lu` is renamed, update `bootstrap.sh`; do not add it back as a Makefile prerequisite. |
| D-0020 | 2026-10-25 | A3 | **Author-directed backend program:** `luc -n` is the primary execution path for Lu v5, the native ELF stays static and syscall-only, and codegen performance is now in scope (supersedes the Phase-1 "no performance work / no codegen improvements" non-goal in charter §7.2). LuMotion stays untouched. | Author directive: «дальше и не трогай motion и развивай независимость и бэкенд и чтоб v5 была единным бэкендом и работало независимо и работало качество на уровне gcc». Evidence and mechanisms in the ADR below. | Re-open on any native/C parity failure, or if the native ELF gains an interpreter or shared-library dependency. |

## ADR D-0020 — Native backend as the primary v5 path

**Status:** accepted 2026-10-25 by Author directive.

**Context.** The Phase-1 charter (§7.2) listed "no performance work, no
codegen improvements" as a non-goal. The Author superseded that in session 5:
develop backend independence, make v5 the single backend, make it work
without external tooling, and reach GCC-level quality. `motion/` (LuMotion) is
explicitly out of scope.

**Decision.**
1. `luc -n` (`src/x86.c`) is the primary execution path for Lu v5. It writes a
   static x86-64 ELF with program headers only — no `PT_INTERP`, no
   `DT_NEEDED`, no section headers — and the program talks to the kernel
   through `syscall`. Building and running it needs no gcc, assembler, linker
   or libc.
2. The C backend (`luc -c`) stays as the differential reference, not the
   product path. `tests/native_test.sh` builds every `tests/native/*.lu` both
   ways, requires identical stdout and exit status, and rejects any native
   output that depends on a dynamic loader or shared library.
3. Codegen quality is an explicit goal, measured against `gcc -O2` on the
   Author's three microbenchmarks (`bench/`): best of five runs, same host.
   Semantic behaviour does not change; the parity suite is the gate.

**Scope note.** "v5 as the single backend" is implemented here as one
canonical native code path for v5 programs, with the C output kept only as a
differential reference. It does not remove Classic parsing: `Lu/Language`
files stay frozen but compilable by charter, and the shipped corpus still uses
them. Retiring the Classic front-end is a separate A3 decision with a corpus
migration attached, not a side effect of this one.

**Evidence** (2026-10-25, host gcc 15.3.0, seconds, best of 5):

| Test | Native | gcc -O2 | Ratio |
|---|---|---|---|
| arith, 20M iterations | 0.0615 | 0.0618 | 1.00x |
| mem, 5M int64 fill + sum | 0.0127 | 0.0101 | 1.26x |
| fib(30) | 0.0066 | 0.0031 | 2.12x |

Compiler: ~1 ms and 12 KB for an empty program. Gate: native 7/7
differential, v5 39 checks, ports 10 checks, doctor 66/66, CLI, `test-all`.

**Mechanisms implemented** (all in `src/x86.c`):

- static register cache r12–r15 for up to four address-not-taken scalars
  (arrays, `auto` and address-taken locals are excluded);
- round-up magic division/modulo with a compile-time error bound, powers of
  two by shift (the old absolute-value sequence was longer and lost the sign);
- compare+branch fusion, cached-register comparisons, register increments;
- indexed addressing with SIB scaling, including base rsp/r12 (disp32 form)
  and rbp/r13 (disp8 form), plus direct indexed stores;
- memory-operand accumulation (`acc += p[i]`) and direct argument-register
  preparation for `f(v ± k)`;
- 16-byte alignment of loop heads.

**Known gap.** `fib` stays 2.1x behind gcc because gcc unrolls its own
recursion eight levels at `-O2`. Matching it needs inlining or recursion
unrolling, which this backend does not implement yet. It is recorded here
rather than hidden.

**Review triggers.** Any native/C parity failure; a native ELF with a dynamic
dependency; a change that would require assembler or libc tooling in the
native path.

## Decisions needed (not yet made)

| # | Question | Options | Recommendation | Default if no answer |
|---|---|---|---|---|
| Q4 | `dv(&x)` is a raw `read(0, &x, sizeof(T))`, not text parsing; the old docs implied numbers. | (a) keep raw semantics and document it; (b) change `dv` to parse text (A3 language change) | (a) — `Uios` `Uin >> x` already covers text input, raw byte reads are useful | stays raw, documented (SPEC U-06) |
| Q7 | Four corpus files cannot be ported without changing output: string building (`test_v13`), the Classic v2.0 feature demo (`test_v20`), `float` (`test_paradigms`, `tests/calculator`). Details in `docs/meta/ports/blocked.md`. | (a) keep the v5 subset; mark these files Classic-only (recommended); (b) grow v5 to cover string building, `float`, `auto`, `Vector`, `Unique`, f-strings (A3, multi-session); (c) unblock only `test_v13` with a specified string-building facility | (a) — each feature already has a recorded absence decision (U-02/U-03/U-04, §4.1, §9); adding them changes the language, not the port | blocked files stay Classic-only; no fake ports |
