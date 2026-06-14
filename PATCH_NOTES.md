# Patch notes: Lu compiler hardening pass

This build is still a small young compiler, but the core is now much more usable for writing real Lu programs.

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
