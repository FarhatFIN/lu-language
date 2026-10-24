# v5-native capability probes (luget, Q-02)

What Phase 2 needs from a Lu/Strict (v5) program compiled with the native
backend (`luc file.lu`): read files, walk directories, see its own arguments,
and spawn `git` with an argument vector. These probes record the current
answer per capability. `docs/meta/phase1-baseline.md` §6 is the summary; this
directory is the evidence.

Run:

```sh
sh tests/probes/luget/run.sh
```

8 checks, all expected to pass on the v4.6 baseline. Requires `luc` at
`src/luc` (build with `make -C src`). The probes read `data.txt` and
`dirwalk_fixture/` relative to their own directory, so run the script, not the
`.lu` files directly.

| Probe | Question | Measured |
|---|---|---|
| `recursion.lu` | How deep can native recursion go? | 20 000 frames ok (control); 100 000 segfaults, rc 139, **no diagnostic**. Bisected: 25 000 ok, 30 000 crash |
| `file_io.lu` | File open/read/close without libc? | Works via `syscall(2)`, `syscall(0)`, `syscall(3)`; prints `bytes= 9` and the contents |
| `dirwalk.lu` | Directory enumeration? | Works: `open(O_DIRECTORY)` + `getdents64` (217), record length decoded from bytes 16–17 |
| `argv.lu` | Can v5 code read its own argv? | **No**: `error: undeclared name '__luc_argc'` |
| `argv_proc.lu` | Workaround for argv? | Works: `/proc/self/cmdline` read through raw `syscall`s, NUL-separated |
| `spawn.lu` | Spawn a process with an argv array? | **No**: `var args: ptr/byte[2]` is a parse error (`unexpected token '['`); no pointer→int cast exists, so `execve` cannot be reached |
| `v5hdr.lu` | Import a C header (FFI) from v5? | **No**: `incl <stdlib.h>` → `include file 'stdlib.h' not found`; v5 special-cases only `tensor.h` |

## Consequences

Two capabilities are missing and both are on luget's critical path:

1. **argv** — v5 code cannot see its arguments (a `/proc` workaround exists
   but is Linux-only and second-class).
2. **spawn with argv** — `git` cannot be invoked: `execve` needs an argv array
   that v5 cannot express, and there is no FFI to reach `system`/`execvp`.

Both feed triage items T-04/T-05/T-06 in `docs/meta/phase1-baseline.md` and
need an A3 decision before Phase 2 can claim "luget is written in Lu v5".
These are characterization probes; once the compiler/stdlib change lands they
should graduate into `tests/conformance/`.
