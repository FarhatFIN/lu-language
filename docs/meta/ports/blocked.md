# Ports blocked by absent v5 features

Four corpus files cannot be translated without changing observable behavior.
They stay in place as Classic originals; no fake ports are filed. Each entry
names the construct, the SPEC rule that makes it impossible, and the
escalation.

## test_v13.lu (src/)

**Blocker: string building (U-11).** The regression exercises
`str + str` and `str + int`:

```lu
Fn/format(str label, int value):str {
    Ret/label + ": " + value
}
...
str greeting = "Hello, " + name + "!"
str labeled = format("Count", 42)
```

SPEC §9 states that `str` has no concatenation operator and that `Uios`/byte
buffers are the replacement. There is no integer-to-string conversion in the
v5 stdlib, so reproducing `format` would mean hand-writing a string builder
and `itoa` inside the test — a rewrite, not a port, and it would test the
builder rather than the language feature the file exists for.

Everything else in the file is portable mechanically: `class` → `struct`,
`enum` → `vid` constants, `new`/`Free` → `Alloc`/`Del`, `Loop/Each` →
`for x in arr`, compound assignment → `set x = x + e`.

**Escalation:** Q7. Proposal: keep v5's subset (recommended) and leave this
file Classic-only; revisit if string building is added deliberately.

## test_v20.lu (src/)

**Blocker: the file is a Classic v2.0 feature demo.** It uses `auto`,
`float`, f-string interpolation, `Vector<int>`, `Unique<int>`, `range()`,
`elif`, `return`, list-literal iteration, and the Classic stdlib
(`sqrt`, `max`, `min`, `abs`, `len`, `upper`, `lower`, `contains`,
`replace`). Each is absent from v5 by a recorded decision: U-02 (f-strings),
U-03 (`auto`), U-04 (`Vector`), SPEC §4.1 (`float`), §12 (`elif`, `return`,
stdlib). A port would have to delete most of the file, destroying its purpose.

**Escalation:** Q7. Proposal: treat the file as a Classic feature demo, not
a port candidate. The v5 suite (`tests/v5/good/*`) covers the constructs v5
does support. If v5 later gains these features, port the demo then.

## test_paradigms.lu (src/)

**Blocker: `float`.** Test 1 declares `float float_val = 3.14159` and prints
it. v5 has no `float` (SPEC §4.1); the output line cannot be reproduced. The
rest of the file (`Process` struct, unused enum, loops, functions, `Alloc`,
`Memset`, `Del`) is portable.

**Escalation:** Q7. Proposal: either leave the file Classic-only (default),
or approve a documented delta that drops the float section and records the
removed output. Do not silently omit the line.

## calculator.lu (tests/)

**Blocker: `float` division.** `Fn/divide(float a, float b):float` and the
line `12 / 4 = ` followed by `3.000000`. Integer division would print `3`
and change behavior. v5 cannot express the function.

**Escalation:** Q7. Proposal: leave the file Classic-only until v5 has a
non-integer type. `src/calculator_gui.lu` is the S4 step-3 target and has the
same dependency; that port needs the same decision first.

## Not blocked, not yet ported

The S4 order continues with the tensor demos, the X11 calculator (step 3) and
LuMotion (step 4). The remaining shipped `src/` programs
(`kernel_advanced.lu`, `kernel_sim.lu`, `messenger.lu`, `native_demo.lu`,
`lu_compiler.lu`) are not part of the unit-test corpus and are deferred until
the designated order is complete.
