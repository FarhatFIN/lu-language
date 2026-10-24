# Diagnostic catalogue v1

Charter Appendix F. This lists the diagnostics `luc 4.8` emits, with a
trigger and the test that pins the wording. Classes:

- **S (stable)** — a test asserts this message; wording is a compatibility
  surface and changes under the deprecation policy.
- **P (provisional)** — emitted today but untested; wording may change until
  a test is added. An untested diagnostic is a diagnostic that can rot.

## Format

Current output (v5 front-end):

```
<file>:<line>: error: <message>
```

Classic semantic warnings: `[LU WARN] line N: <message>`. CLI and include
errors are prefixed `[luc]`. The target format adds the column and the
`rule:` / `note:` / `hint:` lines required by Appendix F; that migration is
U-07 in `SPEC.md` Annex A, not yet done.

## Lexical and syntactic

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-001 | `unexpected character '%c'` | `;` or any non-token byte | S — `tests/v5/bad/13_semicolon.lu` |
| E-002 | `unexpected token '%s'` | wrong token where a construct is expected; also `return`, `asm` | S — `tests/v5/bad/16_missing_brace.lu`, `24_return_keyword.lu` |
| E-003 | `unexpected token '%s' at top level` | statement outside a function, e.g. `Import` | P |
| E-004 | `block required` | `if c ret 0` | P |
| E-005 | `assignment to '%s' requires 'set'` | bare assignment to a declared name | P |
| E-006 | `expected %s before '%s'` / `expected '%s' before '%s'` | generic expect failures | P |
| E-007 | `unexpected token ...` for `>`/`<` mismatches in braces — see E-002 | — | — |

## Names and declarations

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-010 | `undeclared name '%s'` | `x = 5`; `null`; `f"..."` | S — `tests/v5/bad/14_bare_assign.lu`, `25_null.lu` |
| E-011 | `redeclaration of '%s' (shadowing is forbidden)` | inner `vid x` | S — `tests/v5/bad/18_shadowing.lu` |
| E-012 | `redeclaration of function '%s'` | two `fn f` | P |
| E-013 | `missing type for '%s'` | `vid x = 5` | S — `tests/v5/bad/19_missing_type.lu` |
| E-014 | `unknown type '%s'` | `float`, `auto`, `Vector` | P |
| E-015 | `unknown variable '%s'` | `dv(&x)` with no `x` | P |
| E-016 | `unknown function '%s'` | call with no declaration | P |
| E-017 | `unknown method '%s' on '%s'` | method not found on a struct | P |
| E-018 | `unknown field '%s' in '%s'` | field not found | P |
| E-019 | `unknown packed field '%s' in '%s'` | pack field not found | P |
| E-020 | `too many parameters (max 6)` | 7-parameter function | S — `tests/v5/bad/23_too_many_params.lu` |
| E-021 | `global state must be declared immutable with 'vid'` | top-level `var` | P |
| E-022 | `expected a variable name` / `a struct name` / … | `expect_id` variants | P |
| E-023 | `field '%s' of '%s' is private; declare it 'pub' to use it outside the struct` | private field read/write outside the declaring struct's methods | S — `tests/v5/bad/29_private_field.lu` |

## Types and values

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-030 | `cannot assign %s to %s — use an explicit conversion` | `str` + `str`, mismatched assignment | P |
| E-031 | `cannot initialise '%s' with %s` | initialiser type mismatch | P |
| E-032 | `cannot initialise '%s' (%s) with %s — use an explicit conversion` | declaration mismatch | P |
| E-033 | `implicit conversion %s -> %s; write %s(...)` | `int i = b` where `b: byte` | S — `tests/v5/bad/03_implicit_byte.lu` |
| E-034 | `cannot assign to immutable '%s'` | `set` on a `vid` | S — `tests/v5/bad/17_vid_immutable.lu` |
| E-035 | `condition must be bool, found %s` | `if x` with `x: int` | S — `tests/v5/bad/28_condition_int.lu` |
| E-036 | `argument %d of '%s' is %s, expected %s (use an explicit conversion)` | argument type mismatch | P |
| E-037 | `array element %d is %s, expected %s` | array literal element mismatch | P |
| E-038 | `too many elements for '%s' (%d)` | array literal too long | P |
| E-039 | `array length must be an integer literal` | `int[n]` | P |
| E-040 | `'%s' expects %d argument(s), got %d` | built-in arity | P |
| E-041 | `'%s' expects %d element(s), got %d` | built-in element count | P |
| E-042 | `'%s' expects %s, found %s` | built-in argument type | P |
| E-043 | `'%s' is not a pointer` | `Alloc`/`Memcpy` on non-pointer | P |
| E-044 | `'%s' expects a pointer to %s` | pointer target mismatch | P |

## Memory

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-050 | `use of '%s' after Del` | use or second `Del` after `Del p` | S — `tests/v5/bad/27_use_after_del.lu` |

## Control flow

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-060 | `match on %s needs an else branch` | non-exhaustive `match` on `str`/int | S — `tests/v5/bad/22_match_needs_else.lu` |
| E-061 | `expected 'case' or 'else' in match` | malformed match body | P |
| E-062 | `expected 'in' in for loop` | `for x 0..5` | P |
| E-063 | `right side of 'in' must be an array` | `x in 5` | P |
| E-064 | `expected a loop variable` | malformed `for` | P |
| E-065 | `missing return in non-void function '%s'` | a non-`void` function can fall off the end | S — `tests/v5/bad/30_missing_return.lu` |
| E-066 | `missing return in non-void method '%s.%s'` | the same rule for methods | S — `tests/v5/bad/31_missing_return_method.lu` |
| E-067 | `non-void function must return a value` | bare `ret` in a non-`void` function | S — `tests/v5/bad/32_ret_without_value.lu` |
| E-068 | `void function must not return a value` | `ret e` in a `void` function | S — `tests/v5/bad/33_ret_value_void.lu` |

## Bit fields (`pack`)

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-070 | `expected a bit index` | `vid..1` | P |
| E-071 | `bit field must start with vid<N>` | `x0..1` | P |
| E-072 | `expected ':' before '..'` | `vid0..1 lit bool` | S — `tests/v5/bad/15_pack_field.lu` |
| E-073 | `a bit range (vid0..N)` | malformed range | P |

## Input/output

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-080 | `format strings are forbidden: Pr takes values, not a format` | `Pr("%d", x)` | S — `tests/v5/bad/26_format_string.lu` |
| E-081 | `dv expects a reference, e.g. dv(&x)` | `dv(x)` | P |
| E-082 | `dv cannot read into %s` | `dv(&s)` on `str`/array | P |
| E-083 | `cannot stream a %s value` | `Uout << struct` | P |
| E-084 | `operator '%s' needs numbers or a stream` | non-stream `<<` | P |
| E-085 | `right side of '>>' must be a variable, field or element` | `Uin >> 5` | P |
| E-086 | `'>>' reads into int, int64, byte or bool, not %s` | `Uin >> str` | P |
| E-087 | `'%s' has no method for %s with '%s'` | `<<` on an unsupported right type | P |

## Includes

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-090 | `include file '%s' not found` | `incl <no/such/library>` | S — `tests/v5/bad/20_missing_incl.lu` |
| E-091 | `include path must be written as <name>` | `incl foo` | P |
| E-092 | `cannot read include file '%s'` | unreadable file | P |
| E-093 | `%s:1: error: first line must be 'Lu/Strict'` | v5 includes a Classic file | P |
| E-094 | `[luc] %s:%d: incl <%s> — file not found (...)` | Classic missing include (lists the search order) | P |
| E-095 | `[luc] %s:%d: incl <%s> is a Lu/Strict (v5) file — …` | Classic includes a v5 file | P |

## Tensor and backend gating

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-100 | `type 'Tensor' needs \`incl <tensor.h>\`` | `Tensor` without the header | P |
| E-101 | `'%s' needs \`incl <tensor.h>\`` | tensor function without the header | P |
| E-102 | `unknown tensor function '%s'` | unknown `tensor_*` | P |
| E-103 | `[luc] tensor.h needs the C backend: …` | native backend + tensor | P |

## CLI and build

| ID | Message | Trigger | Class / test |
|---|---|---|---|
| E-110 | `[luc] expected exactly one source file` | two positional args | P |
| E-111 | `[luc] invalid optimisation level '%s': expected 0-3` | `-O9` | P |
| E-112 | `[luc] failed to write output '%s'` | `/dev/full` | P (exercised by `tests/cli_test.sh`) |
| E-113 | `[luc] cannot run '%s': %s` | `make` mode exec failure | P |
| E-114 | `[luc] program killed by signal %d` | child died by signal in `make` mode | P |
| E-115 | `[luc] 'make' runs the native backend — it cannot be combined with -c` | `-c … make` | P (exercised by `tests/cli_test.sh`) |

## Open items

- Column numbers and `rule:`/`note:`/`hint:` lines are not implemented
  (U-07). Adding them is a user-visible change and must update every test
  that asserts a message.
- E-002 covers several distinct parser failures; S3.5 should split them so
  each has a stable message and a test.
- Classic-only diagnostics from `semantic.c`/`parser.c` are not catalogued
  here; they are frozen and will be removed with Classic (S8).
