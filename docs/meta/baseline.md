# v4.6 baseline

Recorded 2026-10-25 by LU-ARCH (session 1).

| Field | Value |
|---|---|
| Repository | `FarhatFIN/lu-language` |
| Baseline revision | `4d20343` (`docs: v4.6 — incl in v4, else-if chains, LuMotion`) |
| Baseline tag | `v4.6-baseline` (annotated, local; push pending Author approval) |
| Compiler banner | `Lu Compiler  v4.6` (`./luc -v` → `luc (Lu Compiler) v4.6`) |
| Host | Linux x86-64 (Kali), gcc 15.3.0, git 2.53.0 |
| Build entry | `make -C src` (there is no root Makefile) |
| Backends | native `-n` (default; static ELF), C `-c` (C11 + `cc ... -lm`) |

## Compiler binaries

| State | SHA-256 of `src/luc` |
|---|---|
| HEAD `4d20343`, clean build | `31766d3f00b3448473deb299ff03452a8d77a16f0237336c94da3819c69cb5de` |
| Working tree with D-0001/D-0002 applied | `15185df8106518c42999962fc30af5bd54195b2de793773eac0388566c8432e6` |

Build commands:

```sh
make -C src clean && make -C src
sha256sum src/luc
```

## Compiler source size (lines)

`codegen.c` 3870, `v5.c` 3165, `parser.c` 2906, `x86.c` 2360,
`lu_doctor.c` 1159, `semantic.c` 799, `lexer.c` 763, `lu.h` 496,
`main.c` 424, `includes.c` 378, `util.c` 189.

## Reproduction notes

- No CI exists. `make -C src test-all` is the widest runner; `make -C src
  agent-test` is the C doctor (66 checks).
- `tests/v5_test.sh` runs every v5 program on both backends and diffs stdout.
- The repository has no root `Makefile`, no `SPEC.md`, no `docs/` (this
  directory was created in session 1), no worklog, and no tags before
  `v4.6-baseline`.
