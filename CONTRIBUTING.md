# Contributing to Lu

Lu is consolidating onto one language. The rules below are the short version
of the governance in the project charter; they exist so that review does not
depend on memory.

## Which syntax

New code is written in **Lu v5** (file header `Lu/Strict`). **Lu Classic**
still compiles but is frozen — see [docs/meta/classic-freeze.md](docs/meta/classic-freeze.md).
New language features go to v5 only.

## Changing Lu Classic

A change to a Classic-only path (parser/codegen branch that exists solely for
legacy syntax) is rejected unless it is labelled `classic-critical-fix` **and**
meets both criteria:

1. it fixes a crash, memory unsafety, or security issue;
2. it restores previously correct behaviour without improving behaviour.

Add a `Classic-touch:` footer to the commit message naming the exception.
Exceptions are recorded in the freeze ledger.

## Definition of done

- `make -C src clean && make -C src` is clean.
- `make -C src test-all` is green (it covers CLI, native parity, v5 and the
  shipped programs).
- A behaviour change ships with a test change and a `SPEC.md` update.
- Both backends are considered; `docs/meta/parity.md` is updated when a
  feature's backend support changes.
- The worklog and `docs/meta/decisions.md` are updated.

## Diagnostics

User-facing diagnostics follow the format in `SPEC.md` §10 and are catalogued
in `docs/meta/errors.md`. Wording is a compatibility surface: changing a
stable message updates the catalogue and its test in the same commit.
