# Lu v5 — Language Specification

Status: **v0.2 (Phase 1, S3 hardening)** — normative sections are written
against `luc 4.8`; gaps found while writing are listed in Annex A instead of
being papered over.

This document defines **Lu v5**, the canonical Lu syntax (file header
`Lu/Strict`). Lu Classic is the frozen legacy syntax; normative differences
are in §12. Section numbers are reserved for conformance mapping
(`tests/conformance/MAP.md`).

## 1. Scope and conformance language

Lu v5 is a statically typed, explicitly declared language with two backends:
a native freestanding x86-64 ELF writer and a C11 emitter (§11).

The key words MUST, MUST NOT, MAY are used in the RFC-2119 sense. A MUST
statement is only used where a test in this repository checks it; the test is
named in the same paragraph or table. Behaviour described with "does/is" is
observed on `luc 4.8` but not yet test-linked, and is therefore not a
promise — Annex A tracks the difference.

A **conforming program** is UTF-8 source whose first token is `Lu/Strict` and
which is accepted by the grammar (§3), the static rules (§4–§7) and the
program-structure rules (§8). A **conforming implementation** accepts every
conforming program with the semantics in §5–§9, rejects every program that
violates a MUST NOT, and reports violations in the format of §10.

## 2. Lexical structure

### 2.1 Source encoding

Source is a byte stream. Identifiers are ASCII; string contents and comments
may contain any bytes, including UTF-8 Cyrillic. The compiler does not
validate UTF-8.

### 2.2 Identifiers and keywords

```
ident = ( letter | "_" ) { letter | digit | "_" } ;
letter = "A"…"Z" | "a"…"z" ;
digit  = "0"…"9" ;
```

Cyrillic in identifiers is not an identifier character and is rejected by the
lexer ("unexpected character").

Reserved words (20): `incl, vid, var, set, pub, fn, struct, if, else, while,
loop, for, in, match, case, pack, Del, ret, break, continue`. `true` and
`false` are boolean literals. `Pr`, `PrBits`, `dv`, `Alloc`, `Memset`,
`Memcpy`, `syscall` are built-in names, not keywords.

### 2.3 Literals

| Form | Examples | Type |
|---|---|---|
| Decimal integer | `42`, `1_000_000` | `int` |
| Hexadecimal | `0x2a` | `int` |
| Binary | `0b101010` | `int` |
| String | `"abc"`, `«abc»` | `str` |
| Boolean | `true`, `false` | `bool` |

String escapes are `\n`, `\t`, `\"`, `\\`. A string literal is immutable and
NUL-terminated. There is **no floating-point literal** and **no `null`
literal**; both are rejected (`unknown type 'float'`, `undeclared name
'null'`).

Evidence: `tests/v5/good/01_arith.lu`; session-1 probes (see Annex A).

### 2.4 String interpolation

**Not part of Lu v5 4.8.** `f"n={n}"` is parsed as identifier `f` followed by
a string and rejected with `undeclared name 'f'`. This section is reserved;
if interpolation is added it will specify exactly what is interpolable,
how braces are escaped, and the formatting of numbers and booleans. Tracked
as U-02.

### 2.5 Comments and layout

`//` comments run to end of line; `/* … */` does not nest. A newline ends a
statement. Newlines inside `(`, `[`, `{` are insignificant. Indentation is
not significant; blocks are delimited only by braces.

A semicolon is not a statement terminator and is rejected ("unexpected
character ';'"). Evidence: `tests/v5/bad/13_semicolon.lu`.

## 3. Grammar (EBNF)

This is the accepted surface as of `luc 4.8`, mirroring `SYNTAX.md` §12.
A differential check (parser ↔ grammar) is scheduled in S3.5; divergence is
tracked as U-10.

```ebnf
program   = "Lu/Strict" { item } ;
item      = incl | global | struct | pack | fn ;
incl      = "incl" ( "<" path ">" | STR ) ;
path      = IDENT { "/" IDENT } ;
global    = "vid" IDENT ":" type [ "=" expr ] ;

struct    = "struct" IDENT "{" { field | method } "}" ;
field     = [ "pub" ] IDENT ":" type [ "=" expr ] ;
method    = "fn" IDENT "(" [ params ] ")" [ "->" type ] block ;

fn        = "fn" IDENT "(" [ params ] ")" [ "->" type ] block ;
params    = param { "," param } ;
param     = "self" | IDENT ":" type ;

type      = "ptr/" typeatom | typeatom [ "[" INT "]" ] ;
typeatom  = "byte" | "int" | "int64" | "bool" | "str" | "void" | IDENT ;

block     = "{" { stmt } "}" ;
stmt      = bind | assign | if | while | loop | for | match
          | free | call | ret | "break" | "continue" ;

bind      = ( "vid" | "var" ) IDENT ":" type [ "=" expr ] ;
assign    = "set" place "=" expr ;
place     = IDENT { "[" expr "]" | "." IDENT } ;

if        = "if" expr block [ "else" ( if | block ) ] ;
while     = "while" expr block ;
loop      = "loop" expr block ;
for       = "for" IDENT "in" expr block ;
match     = "match" expr "{" { case } "}" ;
case      = "case" expr block | "else" block ;

pack      = "pack" typeatom IDENT "{" { bitfield } "}" ;
bitfield  = BITNAME ".." INT IDENT ":" typeatom [ "=" LITERAL ] ;
          (* BITNAME = "vid" followed by a decimal index: vid0, vid3, vid20 *)

free      = "Del" place ;
call      = "Pr" "(" [ args ] ")" | "PrBits" "(" expr ")"
          | "dv" "(" "&" IDENT ")" | IDENT "(" [ args ] ")" ;
args      = expr { "," expr } ;
ret       = "ret" [ expr ] ;

expr      = logic_or ;
logic_or  = logic_and { "||" logic_and } ;
logic_and = compare  { "&&" compare } ;
compare   = sum { ( "==" | "!=" | "<" | "<=" | ">" | ">=" | "in" ) sum } ;
sum       = product { ( "+" | "-" ) product } ;
product   = unary { ( "*" | "/" | "%" ) unary } ;
unary     = ( "-" | "!" | "&" ) unary | postfix ;
postfix   = primary { "[" expr "]" | "." IDENT | "(" [ args ] ")" } ;
primary   = INT | STR | "true" | "false" | IDENT | "(" expr ")"
          | "[" [ expr { "," expr } ] "]" ;
LITERAL   = INT | STR | "true" | "false" ;
```

`PrBits(x)` is accepted in statement position. `return` is **not** a v5
keyword (`unexpected token 'return'`); use `ret`.

## 4. Types

### 4.1 Primitive types

| Type | Size | Range / meaning |
|---|---|---|
| `byte` | 1 | unsigned 0…255 |
| `int` | 4 | signed, two's complement |
| `int64` | 8 | signed, two's complement |
| `bool` | 1 | `true`/`false` |
| `str` | 8 | pointer to immutable NUL-terminated bytes |
| `ptr/T` | 8 | pointer to `T` |
| `T[N]` | N × sizeof(T) | fixed-size array |
| `void` | — | function result type only |

There is no `float` type in v5 (`unknown type 'float'`), no `Vector` type,
and no `auto` annotation (U-03/U-04). Primitive sizes are the native ABI
sizes on x86-64 Linux and are what `syscall` sees.

### 4.2 Arrays

`T[N]` is a value type; assigning it copies. Elements are zero-initialised.
Indexing is 0-based; a `str` index yields a `byte` (`s[1]` of `"abc"` is
`98`). Evidence: `tests/v5/good/04_memory.lu`.

### 4.3 Structs and methods

`struct` declares named fields and methods. Fields are **private by default**;
`pub` makes a field visible outside the declaring struct. Reading or writing a
private field anywhere except in a method of the same struct MUST be rejected
("field 'hp' of 'Hero' is private; declare it 'pub' to use it outside the
struct"). A method may access private fields of any value whose type is its own
struct, including through `self`. Methods are always visible. Methods take an
explicit `self` parameter; there is no implicit `this` and no inheritance.
Struct assignment copies the value.

Evidence: `tests/v5/good/07_struct.lu`, `08_methods.lu`, `17_flow.lu`;
`tests/v5/bad/29_private_field.lu`.

### 4.4 Pointers and memory

`ptr/T` is a pointer; there is no null pointer value. `Alloc(n)` allocates
`n` elements of the pointer's target type. A pointer to a struct is
auto-dereferenced for field access and method calls: `p.field` and
`p.method()` are equivalent to `p[0].field` and `p[0].method()`.
`Memset(p, value, n)` and `Memcpy(dst, src, n)` operate on bytes. `Del p`
frees `p` and makes further use of `p` a compile-time error in the same scope
("use of 'p' after Del"). Double `Del` is an error. Evidence:
`tests/v5/good/04_memory.lu`; `examples/v5/test_core_plus.lu`; session-1
probe.

### 4.5 Conversions and `auto`

Conversions are written as calls named by the target type: `int(b)`,
`byte(i)`, `int64(i)`. There are **no implicit numeric conversions**;
`int i = b` where `b: byte` is rejected ("implicit conversion byte -> int").
Evidence: session-1 probe, `tests/v5/bad/03_implicit_byte.lu`.

`auto` is not implemented in 4.8 (U-03); the type annotation is always
required ("missing type for 'x'", `tests/v5/bad/19_missing_type.lu`).

## 5. Expressions and evaluation order

Operator precedence follows §3 from lowest to highest: `||`, `&&`, comparison
(`== != < <= > >= in`), additive, multiplicative, unary, postfix. `&&` and
`||` short-circuit; the native and C backends agree on this
(`tests/native/logic.lu`). `&&`/`||` operands MUST be `bool`.

`Pr(a, b, …)` prints each argument separated by one space and terminates the
line. There are no format strings: `Pr("%d", x)` is rejected ("format
strings are forbidden"), so a `%` has no special meaning.

Evaluation order between arguments and between an assignment's left and right
sides is **unspecified** (U-09): programs MUST NOT depend on it.

## 6. Statements and control flow

- `vid x: T = e` binds immutably; `set x = e` on a `vid` is rejected
  ("cannot assign to immutable 'x'").
- `var x: T = e` binds mutably; `set` is the only way to change it.
  A declaration without `=` initialises to the type's zero value.
- One declaration per statement; shadowing is rejected ("redeclaration of
  'x' (shadowing is forbidden)").
- `if`/`while` conditions MUST be `bool` ("condition must be bool, found
  int"). Blocks are mandatory (`tests/v5/bad/16_missing_brace.lu`).
- `while c { … }` tests before each iteration. `loop N { … }` runs exactly
  `N` times. `for i in a..b { … }` iterates the half-open range; `for x in
  arr { … }` iterates array elements. `break`/`continue` apply to all three.
- `match v { case e { … } else { … } }`. `match` on `str` and on numeric
  types MUST have an `else` branch ("match on str needs an else branch");
  `match` on `bool` MAY omit it. `match` on `str` compares content, not
  addresses.
- `ret e` returns. `ret` without an expression is allowed only in `void`
  functions ("non-void function must return a value"), and `ret e` is rejected
  in a `void` function ("void function must not return a value"). A non-`void`
  function MUST return on every path: falling off the
  end of the body is rejected ("missing return in non-void function 'f'"). The
  analysis is structural and conservative: a block returns when its last
  reachable statement returns; `if`/`else if`/`else` returns when a final
  `else` exists and every branch returns; `match` returns when it has `else`
  and every `case` and the `else` return. `while`, `loop` and `for` may run
  zero times, so a `ret` inside a loop does not count — place a `ret` after the
  loop. The same rule applies to non-`void` methods ("missing return in
  non-void method 'S.m'").
- `Del p` is a statement, not an expression.

Evidence: `tests/v5/good/02_bindings.lu`, `05_pack.lu`, `06_pack_small.lu`,
`09_fn.lu`, `17_flow.lu`; `tests/v5/bad/14_bare_assign.lu`,
`17_vid_immutable.lu`, `18_shadowing.lu`, `30_missing_return.lu`,
`31_missing_return_method.lu`, `32_ret_without_value.lu`,
`33_ret_value_void.lu`.

## 7. Functions and main

`fn name(p: T, …) -> R { … }`. `-> R` MAY be omitted for `void`. A function
has at most 6 parameters ("too many parameters (max 6)"). There is no
overloading ("redeclaration of function"), no default arguments, and no
varargs except built-in `Pr`. `self` is an ordinary first parameter.

`fn main() -> int` is the entry point; its result is the process exit status.
`fn main() -> void` exits 0. Code executes only inside functions; the top
level is declarations only (a global mutable `var` is rejected: "global
state must be declared immutable with 'vid'").

Evidence: `tests/v5/good/09_fn.lu`, `10_main.lu`; session-1 probes.

## 8. Program structure and `incl`

A v5 file MUST begin with the token `Lu/Strict` (comments and blank lines may
precede it). Without it the file is Lu Classic, or is rejected when included
from v5 ("first line must be 'Lu/Strict'").

`incl <name>` (also `incl "name"`) splices another v5 file at the point of
the directive, before type checking. Normative rules, each proven by
`tests/probes/incl/run.sh` (9/9):

1. **Resolution order.** The directory of the including file, then the
   process CWD, `./include`, `../include`, `<dir of luc>/include`,
   `<dir of luc>/../include`, then each `$LU_INCLUDE` entry (colon-separated).
2. **Extension.** `.lu` is appended unless the name already ends in `.lu`.
3. **Include-once.** A file is spliced at most once per translation unit,
   keyed by canonical path (`realpath`). Diamonds and cycles terminate.
   `<d>` and `<./d>` are the same file.
4. **Dialect boundary.** A v5 file cannot include a Classic file, and a
   Classic unit cannot include a v5 file. Both are compile-time errors.
5. **Missing file.** A missing include is a compile-time error and the
   compiler writes no output.
6. **C headers.** In v5 the only supported C header is `tensor.h`
   (`incl <tensor.h>`), which selects the C backend for tensor programs. Any
   other C header, including `stdlib.h`, is rejected ("include file
   'stdlib.h' not found"). There is no general FFI in v5 (U-04/Q-02).
7. `incl <std/io>` (the whole `std/` namespace) is a built-in no-op: `Pr`,
   `PrBits` and `dv` exist without it.

Evidence: `tests/v5/good/12_incl.lu`, `14_include_diamond.lu`;
`tests/v5/bad/20_missing_incl.lu`, `21_included_file_error.lu`.

## 9. Standard library binding

Names below are available to v5 programs without a declaration.

| Name | Signature / form | Semantics | Backends |
|---|---|---|---|
| `Pr` | `Pr(v, …)` | print values separated by spaces, then `\n` | both |
| `PrBits` | `PrBits(v)` | binary digits, width = type width | both |
| `dv` | `dv(&x) -> bool` | raw `read(0, &x, sizeof(T))`; true when bytes were read. **Not** text parsing (U-06) | both |
| `Alloc` | `Alloc(n) -> ptr/T` | allocate `n` elements of `T` | both |
| `Memset` | `Memset(p, byte, n)` | fill `n` bytes | both |
| `Memcpy` | `Memcpy(dst, src, n)` | copy `n` bytes | both |
| `Del` | `Del p` | free | both |
| `syscall` | `syscall(nr, …)` | Linux syscall | native; C backend through libc |
| `Uios` | via `incl <Uios>` | `Uout`, `Uerr`, `Uin`, `<<`, `>>`, `get_line` | both |
| tensor functions | via `incl <tensor.h>` | `tensor_*`, `Tensor` | C only |

`str` has no concatenation operator in v5: `a + b` on `str` is a type error.
Build text with `Uios` or byte buffers (`str[0]` is a NUL byte).

Evidence: `tests/v5/good/11_input.lu`, `13_uios.lu`,
`tests/probes/luget/file_io.lu`, `dirwalk.lu`.

## 10. Diagnostics

Current format for v5 errors:

```
<file>:<line>: error: <message>
```

Warnings from the Classic semantic pass use `[LU WARN] line N: …`. The target
format is the charter Appendix F form with column, `rule:`, `note:` and
`hint:` lines; today only file/line/message exist (U-07). The catalog of
stable messages is `docs/meta/errors.md`. Diagnostic wording is a
compatibility surface: changing a listed message requires updating the
catalogue and its test in the same commit.

## 11. Backend gating

| Capability | Native (`-n`) | C (`-c`) | Class |
|---|---|---|---|
| `byte`, `int`, `int64`, `bool`, `str`, `ptr/T`, arrays | yes | yes | core |
| structs, methods, `pack`, `match`, `incl` | yes | yes | core |
| raw `syscall` | yes | yes (through libc) | core |
| `Uios` | yes | yes | core |
| `tensor.h`, `Tensor` | **no** — `luc` refuses with the C-backend recipe | yes | tensor |
| `float` | not a v5 type | not a v5 type | — |
| `Vector` | not in v5 | not in v5 | — |
| inline `asm` | not in v5 (`unexpected token 'asm'`) | — | asm |
| X11 / C-header FFI | Classic only (`Import "<…>"`) | Classic only | x11/ffi |

`luc` chooses the native backend unless `-c` or a `.c` output name is given.
A program that needs a C-only class MUST be built with `-c`; selecting the
native backend for `tensor.h` produces a diagnostic naming the exact command.

Evidence: `tests/probes/luget/` (Q-02), `tests/v5/good/13_uios.lu`.

## 12. Deltas from Lu Classic (normative)

These statements are normative for porting; each is observable on `luc 4.8`.

| Area | Lu Classic | Lu v5 |
|---|---|---|
| Header | `Lu/Language` (or none) | `Lu/Strict` MUST be first token |
| Metadata | `M/ us *user*`, `Import Russian` | not accepted |
| Program structure | `#qN` … `#qN:end` blocks execute in source order | code executes only inside `fn main`; top level declares |
| Statement forms | `Fn/`, `If/`+`To/`+`Else/`, `Loop/N`, `Loop/While`, `Set/`, `Ret/`, `Pr/`, `Call/` | `fn`, `if`/`else` with braces, `loop N`, `while`, `set`, `ret`, `Pr(…)`, call expression |
| Declarations | `int x = 0` / `def …` | `vid`/`var` + mandatory `: T` |
| Mutation | bare `x = e` | `set x = e`; `vid` is immutable |
| Compound assignment | `x += e` | `set x = x + e` |
| Return | `return e` | `ret e` only |
| Globals | mutable globals possible; `Def:config`/`Def:const` | only top-level `vid` globals |
| Block reuse | `Def:anw(expr)/Lin` re-runs a matching `#q` block | no equivalent; call a function |
| Aggregates | `class`, `enum`, inheritance | `struct` only; no `enum`, no inheritance |
| Methods | implicit `this` | explicit `self` parameter |
| Heap objects | `new` / `Free` | `Alloc` / `Del` |
| Types | `float`, `Vector` available | not available (U-03/U-04) |
| Strings | `str` with library helpers; interpolation absent | `str`, indexing yields `byte`, no concatenation |
| `match` on `str` | compares addresses | compares content |
| C headers | `Import "<…>"` / `incl <…>` → FFI | only `tensor.h`; no FFI |
| `incl` mechanism | text splicing before lexing | token splicing during v5 parse (same search order and include-once) |
| Inline `asm` | supported (C backend) | rejected |
| Shadowing | allowed | rejected |
| Implicit conversions | allowed | rejected |

The statement-form and program-structure rows are exercised by the first S4
port: `examples/classic/example.lu` → `examples/v5/example.lu`
(`make test-ports`).

Classic is frozen: new features land in v5 only (`docs/meta/classic-freeze.md`).

## Annex A — Unspecified or unimplemented inventory

An item here is a known gap, not a rule. Each MUST be closed by a spec
change plus a test before v1.0.

| ID | Gap | Evidence |
|---|---|---|
| U-02 | String interpolation `f"…"` is absent; section 2.4 is reserved | probe: `undeclared name 'f'` |
| U-03 | `auto` type inference is absent; `SYNTAX.md` mentions it only as an idea | probe: `unknown type 'auto'` |
| U-04 | `Vector` is not a v5 type; the charter's S3 text asks for its rules | probe: `unknown type 'Vector'` |
| U-06 | `dv(&x)` performs a raw binary `read`, not value parsing; `SYNTAX.md` §9 example implies text input | probe: `"42\n"` → `668212` |
| U-07 | Diagnostics lack column, `rule:`, `note:`, `hint:` lines (chart. App. F) | `src/v5.c` `verr` |
| U-08 | Native recursion exhausts the stack between 25k and 30k frames with SIGSEGV and no diagnostic | `tests/probes/luget/recursion.lu` |
| U-09 | Integer overflow, division by zero and expression evaluation order are unspecified | no test |
| U-10 | The EBNF in §3 has not been differentially checked against the parser | S3.5 pending |
| U-11 | `str` concatenation is not expressible (`+` is a type error) | probe |
| U-12 | Missing-include diagnostics differ between front-ends; v5 omits the search order tried | `tests/probes/incl`; T-03 |

## Annex B — Conformance map

`tests/conformance/MAP.md` maps every section of this document to the tests
that prove it and lists the sections that still have no test.
