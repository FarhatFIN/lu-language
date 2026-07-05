# Patch notes: Lu compiler hardening pass

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
