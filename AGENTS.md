# Lu development agents checklist

This is not a background service. It is the working checklist used for future Lu hardening passes.

## 1. Lexer agent

Owns:

- token stability
- comments
- strings
- operator conflicts
- no keyword/token hangs

Regression focus:

- `/` division vs `/lang/`
- `/* ... */`
- `// ...`
- UTF-8 strings `«...»`
- names like `exp`

## 2. Parser agent

Owns:

- AST correctness
- no infinite loops
- C-style assignment
- class/method AST
- function calls and forward declarations

Regression focus:

- `Fn/name(...):type { ... }`
- `#qN ... #qN:end`
- `x = expr`
- `obj.field = expr`
- `v.method()`
- `ptr/T p = new T()`

## 3. Semantic agent

Owns:

- declaration collection
- false-warning reduction
- method lookup
- pointer/object distinction
- class scope

Regression focus:

- function declared below call
- class declared below use
- `this.field`
- `v.method()`
- `p.method()`

## 4. Codegen agent

Owns:

- valid C11 output
- type lowering
- class-to-C lowering
- runtime integration
- stable generated `main()`

Regression focus:

- generated C builds with `gcc -std=c11`
- function prototypes emitted before calls
- methods lower to `Type_method(Type *this, ...)`
- pointer dot lowers to `->`

## 5. Test agent

Owns:

- `make test`
- `make test-all`
- `bootstrap.sh`
- regression samples

Minimum release gate:

```bash
make clean
make
make test
make test-all
./bootstrap.sh
```

## Executable C doctor

The checklist above is backed by a plain C regression runner:

```bash
cd src
make agent-test
```

The runner is `src/lu_doctor.c`. It does not use Python and does not run in the background. It creates temporary `.lu` files, calls `./luc`, compiles the generated C with GCC, runs the native binary, and compares stdout.
