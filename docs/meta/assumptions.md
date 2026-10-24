# Assumption ledger

Open `[A-nn]` items (charter §13.4). Each has a basis, a risk, and a
settle-path.

[A-01] The `make test-all` failure comes from the Author's untracked scratch
files, not from the committed corpus.
Basis: `src/gh.lu` and `src/minitest.lu` are untracked (`git status`), and the
suite is fully green with them set aside; both are invalid v5 (`expected '{'
before 'Pr'`, `unexpected token 'else'`). Restored unchanged after the run.
Risk if wrong: if they are intended tests, the corpus baseline is incomplete.
Settles: Author confirms they are scratch; then Q2 (decisions.md) removes the
fragility.

[A-02] `4d20343` is the v4.6 baseline revision, matching the compiler banner
`Lu Compiler v4.6`.
Basis: `git log`, `./luc -v`, README/PATCH_NOTES.
Risk if wrong: tag `v4.6-baseline` points at the wrong revision.
Settles: Author confirms at tag push.

[A-03] The test matrix is representative for the target environment
(Linux x86-64, gcc 15.3.0). The C-backend half is verified only with this
compiler.
Basis: suites ran here on 2026-10-25; no CI exists.
Risk if wrong: C-backend regressions on other compilers/versions go unseen.
Settles: pin the C standard/compiler in SPEC §2.2.7 work (reproducibility) and
run a second compiler (clang) once CI or `make check` exists.

[A-04] `v5` and `Classic` are selected solely by the first non-comment token
(`Lu/Strict` vs anything else), not by any per-construct rule.
Basis: `src/main.c:207`, `src/v5.c:3056`; both dialects accept large C-style
subsets (`motion/*.lu` is `Lu/Language` with zero `Pr/`-style markers).
Risk if wrong: SPEC §12 "deltas" would overstate the syntactic split.
Settles: S3 spec delta pass over both front-ends.
