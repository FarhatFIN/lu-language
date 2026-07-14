# Patch notes: Lu compiler hardening pass

## v3.1 — Full technical audit + On//Spawn fix + Server/Route/Bcast deprecation

### Technical audit (80 steps)

Full codebase audit covering lexer, parser, semantic, codegen, and runtime. 170 fuzz tests run. 7 bugs found and fixed, 8 non-critical issues documented.

**Bugs fixed:**
- `cur()` in parser had no bounds check — OOB read on error recovery
- `peek_tok()` didn't check negative index — OOB read
- `cg_class_resolve_method` infinite loop on cyclic inheritance (A→B→A)
- `emit_c_ident` had 2 inheritance walks without cycle detection
- `new Type(value)` used Lu type names instead of C types (`str` → `str*` instead of `char*`)
- `auto` type inference registered C types (`double`) instead of Lu types (`float`) — f-strings broke on `auto y = 3.14`
- Generated C had 180 unused-function warnings — suppressed via `#pragma GCC diagnostic`

**Exit code fix (from v3.0.1):**
- `g_parse_error_count` global counter — `main()` returns exit 1 if > 0
- Checked after parse, semantic, AND codegen phases
- `luc broken.lu` now correctly returns exit 1 (was 0)

### On/ and Spawn/ — working but not strictly type-safe

Previously these were "known broken" — they generated invalid C that segfaulted. Now:

- **`On/name handler`** — `handler` must be a function name (identifier), not a string. Generates `lu_event_on(ev_name, (void(*)(void*))handler)`. String literals are rejected with a clear error.
- **`Spawn/function`** — `function` must be a function name. Generates `lu_spawn((void(*)(void))function)`. Non-identifier arguments are rejected.

Both produce C that compiles without warnings under `-Wall -Wextra -Wpedantic` and work at runtime. However, the function-pointer cast is **technically undefined behavior** in C if the user's handler signature doesn't exactly match `void(*)(void*)` or `void(*)(void)`. This works on x86-64 because all pointer types share the same representation, but is not portable to platforms where calling conventions differ. The casts suppress the compiler warning; the behavior is correct in practice on the target platform.

### Fuzz testing — reproducible

A fuzz test script (`fuzz_test.sh`) is now included in the repository. It generates random programs and verifies `luc` doesn't crash (segfault) or hang (timeout). Run with:

```
make fuzz           # 100 rounds (default)
make fuzz ROUNDS=500
```

This is a **manual test**, not part of `make agent-test`. It checks crash/timeout resistance only — not correctness of generated C (that's `agent-test`'s job). The 170-round fuzz mentioned in the audit was a one-time manual run; the script in the repo makes this reproducible.

### Server/Route/Bcast — formally deprecated

These v1.x network primitives were marked "experimental, untested" in v2.1.2. After testing during the audit, they were found to generate C that compiles but has no real network functionality (just `printf` stubs). They are now formally **deprecated**:

- `Server #qN cor/{...}` — generates `lu_server_new()` stub, no actual server
- `Route/ dst via gateway` — generates `lu_route()` stub, no actual routing
- `Bcast{payload}` — generates `lu_broadcast()` stub, no actual broadcast
- `Snd{payload}` — generates `lu_send()` stub, no actual networking

Using them produces a deprecation warning. They will be removed in v4.0.

### Build quality

- Compiler: **0 warnings** with `-Wall -Wextra -Wpedantic`
- Generated C: **0 warnings** for simple programs (3 for `example.lu` — unused consts, expected)
- `make agent-test`: **47/47** pass
- `make test-all`: all bundled programs compile and run
- `./bootstrap.sh`: self-hosting verified
- 170 fuzz tests: **0 crashes, 0 timeouts**

---

## v3.0 — C/C++/Zig feature parity

### Added in 7 iterations

**C-core:**
- Multi-dimensional arrays `int m[3][3]`, `int cube[2][2][2]`
- Type casting `(int)3.7`, `(byte)x`
- printf/fprintf with full format specifiers
- Pointer arithmetic `p + 1`, `p += 1`, `p - arr`
- Function pointers `fn(int, int) -> int op = add` (Zig-style)
- `union` type

**C++:**
- Inheritance `class Dog extends Animal` with proper struct layout
- Inherited methods and fields (walks class hierarchy)
- `super.method()` for parent method calls
- Operator overloading `op+`, `op==` — `a + b` → `Vec2_op_add(&a, b)`
- Lambdas `fn(int a, int b) -> int { return a + b }`

**Zig:**
- `defer` — scope-exit cleanup via GCC nested functions + cleanup attribute
- Optionals `?T` — `?int maybe = null` → `int* maybe = NULL`
- `null` literal → `NULL`
- Error unions `!T` — simplified (lowers to T, errors via Try/Catch)
- Slices `arr[1:4]` → `lu_slice_t { ptr, len }`
- Tagged unions via `enum + union + struct` (existing primitives)

**Quality:**
- Error recovery in parser — warn + continue instead of exit(1) on first error
- 17 new regression tests (total: 47)

---

## v2.1.2 — Chan/ parser fix + documented hidden layer

### Critical bug fix (invalid C generation)

`Chan/mychan` generated syntactically invalid C — the variable name was lost:

```c
lu_chan_t * = lu_chan_new();   // ← missing identifier
```

**Root cause:** `parse_statement` for `TOK_CHAN` called `consume_type(p)` to read the channel's "type", but `is_type_tok` includes `TOK_IDENT`, so it swallowed the channel name (`mychan`) as the type. The subsequent `n->sval = cur(p)->value` then grabbed whatever token came next.

**Fix:** Channels are always `lu_chan_t*` — there is no type to parse. The parser now hardcodes the type and reads the name directly:

```c
case TOK_CHAN: {
    consume(p);                          /* eat Chan/ */
    ASTNode *n = node_new(NODE_CHAN_DECL, line);
    n->type_name = lu_strdup("chan");
    n->sval = lu_strdup(cur(p)->value);  /* the channel name */
    consume(p);
    return n;
}
```

The codegen symbol table also now registers the channel as a known pointer, so later references don't warn.

### Bonus fix: `chan <- value` now parses at statement level

The `<-` operator was only recognised inside the legacy `Send/chan <- value` form. The README documents `chan <- value` directly, but the parser treated it as a stray expression statement. Now `mychan <- 42` lowers to `lu_chan_send_safe(mychan, ...)` correctly.

### Documentation: the hidden layer

Lu has a large undocumented surface — network primitives, async, events, debug, and a "game engine" DSL. These were implemented in v1.x but never advertised. This release adds a **"Network, async, debug"** section to README and TUTORIAL covering what actually works:

**Working (tested):**
- `Chan/name` + `name <- value` — channels
- `Event/name` + `Emit/name data` — events
- `Log/level msg` — leveled logging
- `Assert/cond msg` — assertions
- `Try/ { } Catch/ERR_X { } Finally/ { }` — exceptions
- `Throw/ERR_MEM "msg"` — throw

**Known broken (documented as unsupported):**
- `On/name callback` — passes a string where a function pointer is expected → segfault on emit
- `Spawn/expr` — passes an int where a function pointer is expected → segfault on call

These two are now explicitly listed as **not supported** in README rather than silently generating bad C.

### Validation

- New regression tests: `channel_send`, `log_levels`, `event_emit`
- All **30/30** agent tests pass (was 27)
- `make`, `make test`, `make test-all`, `./bootstrap.sh` all pass
- The `messenger.lu` demo still compiles with 0 warnings

---

## v2.1.1 — segfault fix: array indexing in f-strings

### Critical bug fix (segfault)

`f"{arr[i + 1]}"` on an `int` array caused a **segfault**. The generated code was `printf("%s\n", arr[(i + 1)])` — treating the `int` value as a `char*` pointer.

**Root cause:** `expr_kind_from_ast` (the type inference used by f-string codegen) had no `case NODE_EXPR_INDEX` for array indexing. It fell through to `default: return "auto"`, and `"auto"` means "emit as-is assuming char*". So `arr[i+1]` (an int) was passed directly to `lu_str_concat`/`printf` as a `char*`.

**Fix:** Added `NODE_EXPR_INDEX` to `expr_kind_from_ast`. It now:
1. Looks up the base array identifier's registered type (which is the element type)
2. Returns the appropriate kind ("int", "float", "bool", "str")
3. Falls back to "int" as a safe default (int→str conversion via `lu_str_from_int` is always safe — no segfault possible)

This also fixes the same class of bug for these previously-unhandled node types in `expr_kind_from_ast`:
- `NODE_EXPR_INDEX` — array indexing `arr[i]`
- `NODE_EXPR_TERNARY` — ternary `cond ? a : b` (infers from "then" branch)
- `NODE_NEW_EXPR` — `new Type()` (treated as pointer/str)
- `NODE_EXPR_DEREF` — `*p` (treated as int)
- `NODE_EXPR_REF` — `&x` (treated as pointer/str)

### Validation

- New regression tests: `fstring_array_index`, `fstring_array_float`
- All **27/27** agent tests pass
- `make`, `make test`, `make test-all`, `./bootstrap.sh` all pass
- The `messenger.lu` demo still compiles with 0 warnings

---

## v2.1 — f-strings v2 + match/case

This release replaces the string-based f-string implementation with a real AST-based parser, and adds Python 3.10-style `match/case`.

### F-strings v2 (real expression parsing)

The previous f-string implementation used text-based heuristics to determine the type of each `{...}` expression. This worked for simple cases but broke on complex expressions like `f"{obj.method()}"`, `f"{arr[i + 1]}"`, or `f"{p.x}"`.

**v2 fixes this properly:**
- The expression inside `{...}` is now parsed by a **temporary Lexer + Parser** into a real AST node.
- Type inference uses the same `expr_kind_from_ast` logic as the rest of the compiler — no more string heuristics.
- All of these now work:
  ```lu
  f"{p.x}"                    // field access
  f"{obj.method()}"           // method calls
  f"{square(square(3))}"      // nested calls
  f"{x > 40}"                 // comparisons (→ bool)
  f"{1 + 2 * 3}"              // arithmetic
  f"{len(s)}"                 // built-in functions
  ```
- New AST node `NODE_FSTRING` holds the parsed parts (literal text + expression ASTs).
- f-strings now participate correctly in `auto` inference: `auto s = f"x={x}"` → `char*`.
- f-strings work in string concatenation: `f"a={a}" + f"b={b}"`.

### match/case (Python 3.10-style pattern matching)

New `match`/`case`/`default` construct, lowered to an `if/else if` chain:

```lu
match code {
    case 200 { print("OK") }
    case 404 { print("Not Found") }
    case 500 { print("Server Error") }
    case _   { print("Unknown") }
}
```

- `case _` is the default case.
- The matched expression is evaluated once into a temp variable.
- Cases are compared with `==`.
- Works with any integer-like expression.

### Bug fixes from v2.0

- **Global variables**: top-level `NODE_VAR_DECL` is now emitted as `static` before functions, so functions can reference globals (previously globals were emitted inside `main()` and functions couldn't see them).
- **List literal element types**: `[strings]` now generates `char*[]` instead of always `int[]`. The element type is inferred from the first element.
- **`for x in range(n)` inside f-strings**: the loop variable is now registered in the codegen symbol table, so f-strings inside the loop body know its type.
- **`obj.field` type inference in f-strings**: `main_chat.is_group` no longer falsely detected as float (the `.` in `obj.field` was confused with a float literal `3.14`).

### Documentation

- **TUTORIAL.md**: new step-by-step tutorial from Hello World to a full messenger demo.

### Validation

- `make`, `make test`, `make test-all`, `make agent-test` (25/25), `./bootstrap.sh` all pass.
- New regression cases: `fstring_v2_complex`, `match_case`, `match_default`, `fstring_arithmetic`.
- The `messenger.lu` demo now compiles with **0 warnings** (was 12 in v2.0).

---

## v2.0 — Python-style syntax + C++ capabilities

This build is still a small young compiler, but the core is now much more usable for writing real Lu programs.

## v1.3 — bug-fix pass

This release fixes several parser/codegen bugs that could hang the compiler or produce invalid C, and adds two new features.

### Compiler correctness

- **Struct/class with array fields no longer hangs the parser.** The previous build went into an infinite loop on `struct Foo { int arr[5] }` because the `[5]` after the field name was never consumed. Both `parse_struct` and `parse_class` now accept `type name[size]` and `type name[size] = {...}` syntax, and the codegen emits the size into the C struct definition.
- **`Set/this.field[idx] = v` with a multi-token index now parses correctly.** The previous Set/ parser only took a single token as the bracket index, so `Set/this.data[this.top] = v` silently truncated to `this.data[this]`. The parser now consumes the full bracketed expression (with proper depth tracking for nested brackets) and the codegen lowers embedded `this.X` references correctly.
- **`emit_c_ident` now translates `this.`/`super.` anywhere in the lvalue string**, not just at the start. This fixes codegen for `Set/this.data[this.top] = v` and similar expressions where the embedded `this.top` would otherwise leak into the C output as `this.top` (invalid because `this` is a pointer).
- **Compound assignment operators are now supported.** All ten C-style compound forms work: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`. They work on plain variables, struct fields, class fields, and array indices. `x += y` lowers to `x = (x + y)`.

### Type-checker improvements

- **Enum aliases no longer produce false "type mismatch" warnings.** `Color c = RED` now type-checks because enum members are registered with their enum type instead of `int`. Enum↔int is also accepted in either direction.
- **`Loop/Each x in arr { ... }` no longer warns about `x` being undeclared.** The loop variable is now added to the symbol table before the body is checked.

### New features

- **String concatenation with `+`.** When either operand of `+` is a string literal, a `str`-typed variable, or another string concatenation, the compiler emits a `lu_str_concat(a, b)` call instead of invalid `char* + char*` C code. The runtime helper returns a freshly `malloc`'d string. Non-string operands (int, float, bool) are automatically wrapped in `lu_str_from_int` / `lu_str_from_float` / `lu_str_from_bool` so users can write `"Count: " + 42` directly. Examples:
  ```lu
  str greeting = "Hello, " + name + "!"
  str multi = "a" + "b" + "c"
  str with_int = "Count: " + 42
  ```

### Template class methods

- Methods of `template<T>` classes now lower `T` to `void*` in their signatures too (previously only fields were lowered). Calling these methods with non-pointer arguments still requires manual casts — full monomorphization is still future work.

### Version

- Bumped to `1.3` to match README and `luc -v`.

### Validation

- `make`, `make test`, `make test-all`, `make agent-test`, `./bootstrap.sh` all pass.
- New manual test: `struct`/`class` with array fields, `Set/this.field[idx] = v`, all compound operators, enum aliases, string concatenation, and `Loop/Each` body access.

## v1.2 — earlier hardening pass

## Fixed / added in this pass

### Compiler correctness

- Generated C is now emitted in a safer order:
  1. constants / config
  2. structs / enums / classes
  3. function and method prototypes
  4. function and method definitions
  5. `#q` executable blocks
  6. generated `main()`
- Calls to functions declared later now compile under C11.
- Existing bundled Lu programs still compile, build and run.
- `bootstrap.sh` still verifies self-hosting output identity.

### C++-inspired low-level OOP lowering

Lu now has a practical C backend for simple classes:

```lu
class Vec2 {
    int x
    int y
    Fn/sum():int {
        Ret/this.x + this.y
    }
}
```

Generated C shape:

```c
typedef struct Vec2 Vec2;
struct Vec2 {
    int x;
    int y;
};
int Vec2_sum(Vec2 *this);
```

Supported now:

- class fields
- class methods
- `this.field` lowered to `this->field`
- object method calls like `v.sum()` lowered to `Vec2_sum(&v)`
- pointer method calls like `p.sum()` lowered to `Vec2_sum(p)`
- pointer field dot syntax like `p.x = 1` lowered to `p->x = 1`
- simple constructors/destructors lowered as `Type_init(Type *this, ...)` and `Type_deinit(Type *this)`
- simple template class fields lower template type `T` to `void*` for now

### Syntax usability

- C-style assignment is now accepted:

```lu
x = x + 1
obj.field = 10
arr[0] = 7
```

- `ptr/Type p = new Type()` now lowers to `Type *p = calloc(1, sizeof(Type));`.
- `delete name` lowers to `free(name);`.
- C block comments are now supported:

```lu
/* comment */
```

### Tests

Added:

- `src/test_core_plus.lu`
- `make test-all`

`test_core_plus.lu` covers:

- function declared after use
- class declared after use
- method call lowering
- pointer `new`
- pointer dot syntax
- arrays
- division
- block comments
- C-style assignment

Validation performed:

```bash
make
make test
make test-all
./bootstrap.sh
```

All passed.

## Still experimental

These are not yet full production features:

- full C++-style constructors in `new Type(args...)`
- real templates / monomorphization
- virtual dispatch / inheritance ABI
- full exception semantics for `Try/Catch`
- real async scheduler

The stable core is now stronger: functions, blocks, variables, arithmetic, arrays, structs, enums, basic class lowering, methods, pointers, allocation, and generated C build flow.

## C core turnkey pass

Added a real C-only local checking tool:

```bash
cd src
make agent-test
```

`lu_doctor.c` works as three small local agents:

- `syntax-agent`: creates Lu snippets and checks that `./luc` accepts them.
- `c-backend-agent`: checks that generated C11 builds with GCC.
- `runtime-agent`: runs the native binary and compares stdout.

New regression cases covered by `lu_doctor.c`:

- `/` division and `**` power lowering.
- boolean expressions with `&&`.
- enum aliases: `RED` works as well as `Color_RED`.
- array sizes that are expressions.
- real `Try/Catch/Finally` control flow through `setjmp/longjmp`.
- simple classes lowered to C structs and methods.
- `new Type()` / `delete` lowered to `calloc` / `free`.
- language imports like `Import Russian` do not become invalid C includes.

Validation commands run successfully:

```bash
cd src
make
make test
make test-all
make agent-test
make bootstrap
```
