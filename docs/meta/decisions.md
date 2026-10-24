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

## Decisions needed (not yet made)

| # | Question | Options | Recommendation | Default if no answer |
|---|---|---|---|---|
| Q4 | `dv(&x)` is a raw `read(0, &x, sizeof(T))`, not text parsing; the old docs implied numbers. | (a) keep raw semantics and document it; (b) change `dv` to parse text (A3 language change) | (a) — `Uios` `Uin >> x` already covers text input, raw byte reads are useful | stays raw, documented (SPEC U-06) |
| Q5 | `pub` is documentation only: private fields are readable/writable from outside. | (a) enforce access now; (b) keep advisory until v1.0 | (a) before SPEC v1.0 — "private by default" is currently untrue | advisory, flagged as U-05 |
| Q6 | Non-`void` functions have no return-on-every-path check; falling off the end yields an undefined value. | (a) implement in `semantic.c`/v5 checks; (b) leave unspecified | (a), S4 hardening | unspecified (U-01) |
