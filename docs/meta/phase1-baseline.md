# Phase 1 baseline audit (S0)

Session 1, 2026-10-25. Baseline revision `4d20343`; environment recorded in
[baseline.md](baseline.md). Everything below is observed, not remembered:
commands are quoted so they can be re-run.

## 1. Build

```sh
make -C src clean && make -C src
```

Clean build, zero warnings. There is **no root `Makefile`** (charter F-09
assumed one); the build entry is `src/Makefile`. `./luc` and `./luc -v` both
report `v4.6`. Long options (`--version`, `--help`) are not supported;
`./luc --help` prints `invalid option -- '-'` and exits 1. `-v` prints
`luc (Lu Compiler) v4.6`.

The banner did not state the syntax generation at baseline, contrary to
charter §6.1. Fixed in S1 (`luc 4.7 (Lu v5 canonical; Lu Classic frozen)`,
per-compilation dialect in the compile line).

## 2. Test matrix

| Runner | Result |
|---|---|
| `make -C src test` | pass (example.lu → C backend → run) |
| `make -C src test-cli` | pass (`all checks passed`) |
| `make -C src test-native` | pass (5/5 parity: core, logic, loops, memory, syscalls) |
| `make -C src test-v5` | pass (24/24; 22 pre-existing + 2 regressions added in session 1) |
| `make -C src agent-test` | pass (66/66) |
| `make -C src test-all` | pass (17/17 bundled programs) **with caveat below** |
| `make -C src test-gui` | pass (compiles, links `-lX11`; not run — needs a display) |
| `make -C src test-motion` | pass (10/10, incl. byte-identical frame rerender) |
| `tests/probes/incl/run.sh` | pass (9/9) |

**Floor caveat (triage T-01).** `test-all` iterates `*.lu` in `src/`:

```make
for f in *.lu ../tests/*.lu; do ...
```

The Author's untracked scratch files `src/gh.lu` and `src/minitest.lu` are
invalid programs and abort the run at `gh.lu`. With those two files set aside
(not deleted, restored afterwards) the corpus is green. This is a harness
fragility, not a corpus failure: any scratch `.lu` in `src/` breaks the floor.
Recommendation: iterate an explicit program list (decisions.md Q2).

One pre-existing compiler warning surfaces in `make test`:

```
[LU WARN] line 17: Lin reference to '2+2' — no block with matching Pr/ found
```

`example.lu` is Classic and mixes a `Lin` reference with no matching block.
Baseline fact; triaged as T-02 (port/wording work at S4).

## 3. Legacy-usage inventory

Dialect is decided by the first non-comment token: `Lu/Strict` → v5,
everything else → Classic (`src/main.c:207`, `src/v5.c:3056`).

| Area | Files | Lines | Dialect |
|---|---|---|---|
| `motion/` (LuMotion) | 3 | 2285 | Classic |
| `src/` apps & tests | 14 | 1712 | Classic |
| `tests/` (non-v5) | 7 | 364 | Classic |
| `src/lu_compiler.lu` | 1 | 95 | Classic |
| **Classic total (tracked)** | **25** | **~4462** | |
| `include/Uios.lu` | 1 | 315 | v5 |
| `tests/v5/` | 24 | ~330 | v5 |
| **v5 total (tracked)** | **25** | **~645** | |

Classic is ~7× the v5 corpus. All shipped examples and apps are Classic,
confirming F-05. Important nuance: most Classic files use the *C-style long*
syntax (`fn`, `var`, `return`, `if`) — `motion/*.lu` has **zero** `Pr/`,
`If/`, `Set/`, `Ret/`, `#q` markers. The split is by declaration header and
front-end, not by the picture of syntax the docs draw. Old-style markers are
concentrated in `test_paradigms.lu` (73), `kernel_advanced.lu` (68),
`kernel_sim.lu` (55), `messenger.lu` (31), `tests/calculator.lu` (30).
**Consequence for SPEC §12:** "deltas from Classic" must be computed against
the two front-ends, not against a list of old tokens (A-04).

Untracked Author scratch (not inventoried): `src/ff.lu`, `src/gh.lu`,
`src/minitest.lu`, `src/tect.lu`, `bench/*.lu`, `BENCHMARK.md`.

## 4. `incl` semantics (Q-04) — settled by probe

Runner: `sh tests/probes/incl/run.sh` → 9/9 pass. Probes live under
`tests/probes/incl/` and cite their expectations in `run.sh`.

Normative facts as implemented (`src/includes.c`, `src/v5.c`):

1. **Syntax.** `incl <name>` recognized at the start of a line (leading
   spaces/tabs allowed); `incl "name"` also accepted by the v5 front-end.
   Expansion/splicing happens before lexing (Classic) or at token level (v5).
2. **Search order** (both front-ends): directory of the including file →
   process CWD → `./include` → `../include` → `<dir of luc>/include` →
   `<dir of luc>/../include` → `$LU_INCLUDE` (colon-separated). Probes prove
   the including file's directory beats the CWD, and that CWD is the fallback.
3. **Extension.** `.lu` is appended unless the name already ends in `.lu`.
4. **Include-once.** A file is spliced at most once per translation unit, keyed
   by canonical path (`realpath`). This makes diamonds and cycles terminate.
   Probes: diamond, diamond across `<d>`/`<./d>` (Classic and v5), `a↔b` cycle.
   Session 1 fixed v5 to match Classic (D-0002); v5 previously spliced
   `<./d>` and `<d>` twice and rejected valid diamonds.
5. **Diagnostics.** Missing include exits 1. Classic lists the search
   locations; v5 prints only `include file 'X' not found`. Divergence →
   T-03 (error catalogue, S3).
6. **Dialect boundary.** A Classic unit cannot include a v5 file
   (`... is a Lu/Strict (v5) file — v4 units can only include Lu/Language
   files`); a v5 unit cannot include a Classic file (`first line must be
   'Lu/Strict'`). Both rejected cleanly, no hang.
7. **C headers.** Classic passes `incl <foo.h>` through as `Import "<foo.h>"`,
   which makes arbitrary C functions callable on the C backend (verified:
   `Import "<stdlib.h>"` + `system("echo ...")` works). **v5 only special-cases
   `tensor.h`**; `incl <stdlib.h>` fails with "include file not found".
   Consequence: no FFI from v5 (see §6).

Cycle safety: no hang observed at any depth; Classic additionally has a
depth-32 guard, v5 relies on include-once.

## 5. Self-hosting (Q-01) — answered: no

`./bootstrap.sh` passes its fixed-point check:

```
--- Step 7: Bootstrap verification ---
BOOTSTRAP VERIFIED - output is identical
```

But this does **not** mean `luc` compiles itself. `src/lu_compiler.lu`
(95 lines, Classic) is a shim: it reads argv via `__luc_argc`, calls
`lu_self_lex_cast` and `lu_self_codegen_cast` (C helpers in
`lu_self_runtime.h`) for the actual lexing and codegen, and prints a banner.
The bootstrap compiles that 95-line program, not the ~13k lines of C that
make up `luc`. Q-01 verdict: **no full self-hosting; the self-hosted source
is a toy front-end.** Treat "self-hosting support exists" (F-08) as not
verified in any meaningful sense.

## 6. v5 native subset for luget (Q-02) — first cut

Probes under `tests/probes/luget/`: runner `sh tests/probes/luget/run.sh`
→ 8/8 pass on this baseline; commands and measured values are recorded in
`tests/probes/luget/README.md`.

| Capability | native `-n` | C `-c` | Evidence |
|---|---|---|---|
| Recursion | up to ~25k frames; **segfault** at ~30k, no diagnostic | 1M frames ok (tail call optimized by gcc) | `recursion.lu` (bisected 10k/15k/20k/25k ok, 30k crash) |
| File open/read/close | yes, raw `syscall` | yes, `syscall` and `read_file`/`write_file` prelude | `file_io.lu` (`bytes= 9\thello-lu`) |
| Directory enumeration | yes: `open(O_DIRECTORY)` + `getdents64` (217) + byte arithmetic | not probed; same syscalls available | `dirwalk.lu` lists a fixture dir correctly |
| Own argv | no: `__luc_argc` undeclared | no in v5 (prelude defines it only for Classic output) | `argv.lu` |
| argv workaround | `/proc/self/cmdline` readable via syscalls; NUL-separated parsing workable | same | `argv_proc.lu` prints argv[0] |
| Process spawn with argv | **no**: `var args: ptr/byte[2]` is a parse error, no pointer→int cast, so an argv array cannot be built; raw `execve` unreachable in practice | **no**: v5 cannot import C headers, and unknown calls are errors without one | `spawn.lu`; `incl <stdlib.h>` fails |
| FFI / C header import | n/a | Classic only (`Import "<...>"`); v5 has no `Import` keyword | `v5hdr.lu` fails at `incl <stdlib.h>` |
| String ops | manual byte loops/indexing; no libc string API | same | `include/Uios.lu` is the working model |

**Verdict.** The native backend is capable enough for luget's own data work:
TOML parsing, tree walking, hashing, file I/O are all implementable in v5 via
byte buffers and `syscall`. Two capabilities are missing and both are on
luget's critical path:

1. **argv** — v5 code cannot see its own arguments (workaround: `/proc`).
2. **spawn with argv** — `git` cannot be invoked; `execve` needs an argv array
   that v5 cannot express, and there is no FFI to reach `system`/`execvp`.

Consequence for §10.10: "luget is written in Lu v5" is not achievable today
without a compiler/stdlib change. A3 candidate (Phase 1 finding, not Phase 2
scope): argv access and either an argv-array spawn builtin or a general
C-header import for v5. Until ratified, Phase 2 cannot start (11.0 entry
condition additionally depends on G-PH1).

## 7. Deltas from the charter's baseline facts

| Charter fact | Reality |
|---|---|
| F-09 `make` at root produces `luc` | No root Makefile; `make -C src` |
| F-09 `./luc file.lu -o out` native, `-c` C | Correct; `.c` output suffix also selects C backend |
| F-01 "two syntaxes coexist" | Accurate, but both accept large C-style subsets; the selector is the header, not the construct set |
| F-05 all examples/apps Classic | Confirmed (motion, calculator, tests) |
| F-06 `incl` unguarded | **Wrong**: include-once exists via canonical paths in both front-ends; probes prove diamonds/cycles terminate |
| F-07 network stubs deprecated | Not re-verified in session 1 |
| F-08 self-hosting "completeness unverified" | Verified as **not** self-hosting (§5) |
| §6.1 banner format | Not implemented; banner has no generation marker |
| 5.2 Q-04 "repeated inclusion, relative paths" | Settled (§4) |

## 8. Triage items opened

| ID | Item | Priority |
|---|---|---|
| T-01 | `test-all` globs `src/*.lu`; scratch files break the floor | P1 (blocks a clean `make check`) |
| T-02 | `example.lu` emits a `Lin`/`Pr/` warning | P3 (S4 port) |
| T-03 | `incl` missing-file diagnostics differ between front-ends; v5 omits the search order | P2 (S3 error catalogue) |
| T-04 | v5 cannot import C headers; only `tensor.h` is special-cased | P1 (Q-02 blocker; S3/ADR) |
| T-05 | v5 has no argv; Classic/C-backend does via prelude | P1 (Q-02 blocker; S3/ADR) |
| T-06 | v5 cannot express an argv array / pointer cast; no spawn | P1 (Q-02 blocker; S3/ADR) |
| T-07 | Native recursion segfaults ~30k with no diagnostic | P2 (document in SPEC Annex A; not luget-critical) |
| T-08 | A bare `{ … }` block in Classic is emitted in reverse order on the native backend and produces invalid C on the C backend (`printf` outside a function). Found while looking for a scoping wrapper for v5 range-for. | P3 (Classic frozen; v5 lowering avoids bare blocks and uses a synthetic `if true` block) |
