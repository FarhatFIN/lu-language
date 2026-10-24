# Lu Classic freeze ledger

Charter §6.3. Lu Classic (the v4.x-and-older syntax, selected by any header
other than `Lu/Strict`) is frozen: it compiles, it gets no new features, and
it is scheduled for retirement.

| Field | Value |
|---|---|
| Freeze revision | `4d20343` (tag `v4.6-baseline`) |
| Freeze declared | 2026-10-25 (Phase 1, session 1) |
| Retirement plan | Phase 1 step S8 — charter §7.11; mechanism (delete / `--classic` flag / separate binary) is an A3 decision the Author has not yet ratified |
| Migration window | 1–2 compiler releases (§6.2); concrete release numbers to be proposed at S7 |
| Exceptions granted | none |

## Exceptions

Every exception must be justified against the two criteria in §6.3 and
labelled `classic-critical-fix`: (a) crash, memory unsafety, or security;
(b) no behaviour improvement, only restoration of previously correct
behaviour.

| Date | Commit | Justification | Approved by |
|---|---|---|---|
| — | — | — | — |

## Enforcement

- New language features land in Lu v5 only.
- Classic-only parser/codegen paths change only under an exception above.
- `SPEC.md` §12 records the normative deltas; `tests/probes/incl/` and the
  Classic corpus are the differential baseline until retirement.
