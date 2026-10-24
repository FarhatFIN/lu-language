# SPEC conformance map

Maps every section of [`SPEC.md`](../../SPEC.md) to the tests that prove it.
A section without a test is a claim without proof (D3).

Legend: **covered** — every normative statement has a test; **partial** — the
main rule is tested, some statements are not; **open** — no test yet.

| SPEC | Subject | Tests | Status |
|---|---|---|---|
| §1 | Scope / conformance terms | — (document) | n/a |
| §2.1 | Encoding | `tests/v5/good/11_input.lu` (UTF-8 string contents) | partial (non-ASCII identifiers untested) |
| §2.2 | Identifiers / keywords | all `tests/v5/good/*` | partial (keyword table not pinned) |
| §2.3 | Literals | `tests/v5/good/01_arith.lu` | partial (hex/bin/underscore literals untested) |
| §2.4 | String interpolation | — | open (feature absent, U-02) |
| §2.5 | Comments / layout / `;` | `tests/v5/bad/13_semicolon.lu` | partial (comment forms untested) |
| §3 | Grammar | `tests/v5/good/*`, `tests/v5/bad/*` | partial (differential check U-10) |
| §4.1 | Primitive types | `01_arith`, `02_bindings`, `04_memory` | partial (`int64` untested) |
| §4.2 | Arrays | `04_memory`, `16_loops` | partial (str indexing untested) |
| §4.3 | Structs / methods / `pub` | `07_struct`, `08_methods` | partial (`pub` not enforced, U-05) |
| §4.4 | Pointers / `Del` | `04_memory`, `bad/27_use_after_del` | covered |
| §4.5 | Conversions / `auto` | `bad/03_implicit_byte`, `bad/19_missing_type` | partial (`auto` absent, U-03) |
| §5 | Expressions / precedence | `01_arith`, `tests/native/logic.lu` | partial (`in` untested in v5) |
| §6 | Statements / control flow | `02_bindings`, `15_match`, `16_loops`, `bad/14,16,17,18,22,28` | covered |
| §7 | Functions / `main` | `09_fn`, `10_main`, `bad/23_too_many_params`, `bad/24_return_keyword` | partial (path-return check absent, U-01) |
| §8 | `incl` | `12_incl`, `14_include_diamond`, `bad/20,21`, `tests/probes/incl/run.sh` (9/9) | covered |
| §9 | Stdlib binding | `11_input`, `13_uios`, `tests/probes/luget/run.sh` (8/8) | partial (`dv` semantics U-06; tensor only in `tests/cli_test.sh`) |
| §10 | Diagnostics | `tests/v5/bad/*.err`, `tests/cli_test.sh` | partial (catalogue has P entries) |
| §11 | Backend gating | `tests/native_test.sh`, `tests/probes/luget/run.sh`, `tests/cli_test.sh` (tensor) | partial (X11/asm rejection untested) |
| §12 | Deltas from Classic | `tests/probes/incl/interop`, `bad/24_return_keyword` | partial (table not fully test-linked) |
| Annex A | Open items | each has a probe in the session log or `tests/probes/` | tracked |
| Annex B | This map | — | n/a |

## Gaps to close next

1. U-01: add a negative test once the missing-return check exists; until then
   §6/§7 must not claim it.
2. Literal forms: one small test for `0x`, `0b`, `1_000`, comment forms.
3. `int64` arithmetic and `int64` conversion.
4. v5 `in` operator (array and range).
5. `pub` enforcement (compiler change, then `bad/` fixture).
6. X11 / `asm` / `Import` rejection fixtures for §11–§12.

Every new normative statement in `SPEC.md` MUST add a row here in the same
commit.
