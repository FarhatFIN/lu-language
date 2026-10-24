# `incl` probes

Behavioral probes for `incl <...>` (Q-04). They record what the compiler
actually does; once SPEC.md section 8 exists, the ones that pin normative
behavior graduate into `tests/conformance/`.

Run:

```sh
sh tests/probes/incl/run.sh
```

Requires `luc` at `src/luc` (build with `make -C src`).

| Probe | Question |
|---|---|
| `resolve-order` | Is the including file's directory searched before the process CWD? |
| `once` | Are diamonds and cycles included once? Same answer for `incl <d>` vs `incl <./d>`? |
| `missing` | Does a missing include fail with the search order listed, without hanging? |
| `interop` | Can a Classic unit include a v5 file, and a v5 unit include a Classic file? |
