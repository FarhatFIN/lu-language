# Patch notes: Lu compiler hardening pass

## v4.8 — struct privacy and return checking

Two gaps that SPEC v0.1 recorded as unimplemented are now rules with tests.

**Private fields.** A field without `pub` is visible only inside the methods of
the struct that declares it. Reading or writing it elsewhere is rejected:
`field 'hp' of 'Hero' is private; declare it 'pub' to use it outside the
struct`. Methods stay public. Every in-tree v5 struct already marked its
fields `pub`, so no corpus change was needed.

**Missing return.** A non-`void` function or method that can fall off the end
is rejected: `missing return in non-void function 'f'`, `missing return in
non-void method 'S.m'`. The analysis is structural: an `if` chain needs a final
`else` and every branch must return; `match` needs `else` and every branch must
return; loops count as zero iterations, so a `ret` inside a loop does not
satisfy the check. `ret` and the result type must also agree: a bare `ret` in a
non-`void` function and `ret e` in a `void` function are rejected.

**Native backend: independent and competitive.** `luc -n` stays a static
x86-64 ELF with no interpreter, no shared libraries, no libc and no external
assembler or linker; `tests/native_test.sh` now refuses any output that has a
dynamic dependency. Code generation got a static register cache (r12–r15 for
up to four address-not-taken scalars), branch/compare fusion, SIB-scaled
indexed addressing for loads and stores, memory-operand accumulation
(`acc += p[i]`), direct argument preparation (`f(v - k)`), a shorter
round-up magic sequence for constant `/` and `%`, and 16-byte-aligned loop
heads. Against `gcc -O2` on the Author's microbenchmarks (best of five):
arith 20M — 0.0615 s vs 0.0618 s (1.00x); mem 5M — 0.0127 s vs 0.0101 s
(1.26x); fib(30) — 0.0066 s vs 0.0031 s (2.12x). The `fib` gap is gcc's
eight-level recursion unrolling; this backend does not inline yet. Two
differential regressions were added: `tests/native/divmod.lu` (constant and
runtime signed division/modulo across sign and magnitude edges) and
`tests/native/indexregs.lu` (indexed access with the pointer in r12/r14 and
the index in r13 — a SIB-form bug that previously stored the address instead
of the value). The rationale is recorded in ADR D-0020.

**SPEC v0.2.** §4.3 and §6 are normative, Annex A loses U-01 and U-05;
`docs/meta/errors.md` gains E-023/E-065–E-068 and promotes six previously
provisional messages to stable (they already had tests).

**Tests.** `test-v5` grew from 33 to 39 checks: `good/17_flow`,
`bad/29_private_field`, `bad/30_missing_return`,
`bad/31_missing_return_method`, `bad/32_ret_without_value`,
`bad/33_ret_value_void`.

**First S4 ports.** Five shipped programs now have v5 translations in
`examples/v5/`, with the Classic originals preserved in
`examples/classic/`: `example`, `showcase`, `test_full`, `test_core_plus`,
`test_comprehensive`. All five produce byte-identical stdout and exit status
on the C and native backends. `make test-ports` compiles each port and its
Classic original, runs both, and compares the results according to
`tests/ports/backends.txt`; it is wired into `test-all`. `example.lu`'s inert
`Def:anw(2+2)/Lin` reference is dropped, so the `Lin` warning (T-02) no
longer applies to the shipped example. Four corpus files (`test_v13`,
`test_v20`, `test_paradigms`, `tests/calculator`) cannot be ported without
changing output because v5 deliberately lacks string building, `float`, and
the v2.0 feature set; they stay Classic-only pending Q7
(`docs/meta/ports/blocked.md`). SPEC §12 now lists every Classic construct
the ports had to translate, and §4.4 states that `p.field`/`p.method()`
auto-dereference a pointer to a struct. Port reports:
`docs/meta/ports/*.md`.

**Build fix.** `make bootstrap` no longer lists `lu_compiler.lu` as a
prerequisite. With a stale `lu_compiler.lu.c` present, GNU make's built-in
`%: %.c` rule compiled that artifact over the source file. Found while
running the S4 gate; the source was restored from git and the bootstrap
fixed-point check re-verified.

## v4.7 — Lu v5 canonical, SPEC v0.1

Consolidation starts here: the language has one canonical name and one
normative document, and the compiler says which syntax it is compiling.

**Banner and naming.** `luc 4.7 (Lu v5 canonical; Lu Classic frozen)`;
every compilation reports `(Lu v5, O2)` or `(Lu Classic, O2)`. The literal
file header `Lu/Strict` is unchanged — it names the format, not the language.
README, GUIDE, SYNTAX, TUTORIAL and PATCH_NOTES use "Lu v5" / "Lu Classic"
only. README gained a "Какой синтаксис выбрать" section; the freeze policy
lives in `CONTRIBUTING.md` and `docs/meta/classic-freeze.md`.

**`SPEC.md` v0.1.** Sections 1–12 plus Annex A (unimplemented or unspecified
inventory) and Annex B (conformance map). Things the old syntax document
promised but the compiler did not do are recorded as gaps, not rules:
missing-return analysis, `pub` enforcement, `auto`, `Vector`, string
interpolation, and column/`rule:`/`note:`/`hint:` diagnostics.
`docs/meta/errors.md` catalogues every diagnostic v5 emits and marks which
wording a test pins.

**`dv(&x)` documented honestly.** It is a raw `read(0, &x, sizeof(T))`, not
text parsing: `dv(&n)` on `int` reads four machine-order bytes. Use `Uin >> n`
for numbers. The SYNTAX example now shows a byte read.

**Bug fixes.**

- v5 range-for leaked its synthesised names into the enclosing function:
  two `for i in 0..n` loops made the C backend emit `int i` twice
  ("redefinition of 'i'"). The lowering now wraps the range loop in its own
  block. Regression: `tests/v5/good/16_loops.lu`.
- v5 `incl` diagnostics could print heap garbage after the include path was
  freed (session 1) — fixed by keeping spliced paths alive.
- v5 include-once now canonicalises paths with `realpath` like Classic, so
  `<d>` and `<./d>` are one file (session 1).

**Tests.** `test-v5` grew from 22 to 33 checks: match/strings, loops, and
negative fixtures for `match` exhaustiveness, parameter limit, `return`,
`null`, format strings, use-after-`Del`, non-bool conditions. `test-all`
iterates an explicit list of shipped programs, so a scratch file in `src/`
can no longer break the floor.

## v4.6 — `incl` в v4 и первое приложение: LuMotion

До этой версии `incl` существовал только в транспайлере v5: файл вставлялся
потоком токенов на уровне ядра, поэтому основному синтаксису он был
недоступен. Любая программа на `Lu/Language` оставалась одним файлом — а
GUI-приложение, которое нельзя разложить на библиотеку и фронтенды, быстро
превращается в свалку.

**`incl` в v4.** Раскрытие идёт на уровне текста, до лексинга
(`src/includes.c`), поэтому дальше компилятор не знает, что файл был не один:

```lu
incl <core.lu>          // исходник на Lu — вставляется в программу
incl <X11/Xlib.h>       // C-заголовок — становится #include
```

- `.lu` (или имя без расширения) ищется в каталоге включающего файла, `.`,
  `include`, `../include`, рядом с `luc` и в `$LU_INCLUDE`.
- `.h` превращается в `#include <...>`: заголовок видит C-бэкенд.
- Include-once сравнивает **канонические** пути (`realpath`), иначе
  `core.lu` и `./core.lu` были бы разными файлами и «ромб» из двух
  библиотек давал бы повторные определения. Цикл `a → b → a` просто
  останавливается на уже виденном файле.
- Первая строка подключаемого файла (`Lu/Language`) отбрасывается: это
  маркер файла, а не часть программы.
- Файл, начинающийся с `Lu/Strict`, в v4-программу не вставляется — его
  нужно компилировать как v5. Ошибка называет файл и строку `incl`.
- Отсутствующий файл — ошибка компиляции:
  `[luc] bad.lu:3: incl <nope.lu> — file not found (...)`.
- `std/*` остаётся встроенным no-op: `Pr`, `dv` и прочее уже в ядре.

**`else if` исправлен.** Цепочка веток разбиралась как два независимых `if`,
и `else` привязывался ко второму из них — то есть `if a { } else if b { }
else { }` выполнял `else`-ветку при `a == true` и `b == false`. Теперь это
обычное вложение `if` внутрь `else` (три места в `src/parser.c`), и
`GUIDE.md` больше не рекомендует обходить это вложенными скобками.

**`motion/` — LuMotion.** Приложение моушн-графики: редактор на X11 и
headless-рендерер, около 2300 строк на Lu.

- `core.lu` — модель (`Comp`, `Layer`, `Pose`, `Key`), интерполяция с тремя
  режимами ease, аналитический растеризатор с AA через обратное аффинное
  преобразование, шрифт 5×7 на 52 глифа, формат `.lum`, демо-сцена.
- `render.lu` — `--still`, `--seq`, `--info`, `--write`, `--demo`; пишет
  бинарный PPM и ничего не знает про X.
- `motion.lu` — окно 1200×760, один софтверный фреймбуфер и один
  `XPutImage` за перерисовку: транспорт, список слоёв, инспектор, таймлайн
  с ромбами кейфреймов, превью 1:1. Правка позы — auto-key.
- `test.sh` — детерминированные проверки: сборка обоих бинарников,
  состав демо-сцены, заголовок и размер PPM, побайтовая повторяемость
  кадра, зажим номера кадра, диапазон `--seq`, round-trip
  `--write` → загрузка, диагностика на плохой ввод.

Сборка приложения показывает, зачем нужны обе возможности: `incl` даёт
библиотеку сцены, а исправленный `else if` — читаемые цепочки условий в
обработчике клавиш (до фикса они писались вложенными блоками на 4 уровня).

## v4.5 — рабочий `tensor.h` и тензоры из v5

Библиотека тензоров перестала быть набором из шести функций и стала
пригодной для настоящей линейной алгебры — а главное, до неё наконец можно
дотянуться из основного синтаксиса.

**`incl <tensor.h>` в v5.** `tensor.h` — это C-заголовок, а не `.lu`-файл,
поэтому он не вставляется в поток токенов. Транспайлер ставит в
сгенерированный v4-текст `#include <tensor.h>` и заодно узнаёт тип `Tensor`,
тип `float` (это C `double`, поэтому `type_size` для него 8, а не 4) и
сигнатуры `tensor_*` из той же таблицы `src/libraries.c`, по которой работает
семантический анализ v4. Арность и тип результата проверяются на компиляции:
`'tensor_zeros' expects 2 argument(s), got 1`. Без `incl` диагностика тоже
говорит по делу: `type 'Tensor' needs \`incl <tensor.h>\`` и
`'tensor_zeros' needs \`incl <tensor.h>\``. Реализация остаётся в заголовке и
в сгенерированный C не попадает.

Float-литералов в v5 нет (`3.0` — это `3` `.` `0`), поэтому скаляры
передаются целыми: `tensor_full(2, 2, 3)`, `tensor_clip(x, 0, 4)`. C приводит
`int` к `double` при вызове, так что это не костыль, а обычное правило
прототипа. Значения `float` печатаются через `Pr` (там `%g`), тензоры —
через `tensor_print`: `Uios` float-перегрузки не имеет, и смешивать
буферизованный `printf` с небуферизованным `write(2)` в одном потоке нельзя —
вывод переупорядочится.

**Нативный бэкенд** на попытку собрать программу с `tensor.h` теперь отвечает
не «unknown type 'Tensor'», а сразу командами, которые нужны:

```
[luc] tensor.h needs the C backend:
      luc program.lu -c -o out.c
      cc -std=c11 -Iinclude out.c -lm -o program
```

**Новые функции.** `tensor_copy` (глубокая копия: присваивание копирует
только handle), `tensor_matvec` (`[r,c] x [c,1] -> [r,1]`), `tensor_dot`
(скалярное произведение равных форм), `tensor_max`, `tensor_min`,
`tensor_argmax` (плоский row-major индекс первого максимума), `tensor_abs`,
`tensor_tanh` и `tensor_clip(a, low, high)`.

**`tensor_matmul` переписан** в порядке i-k-j: внутренний цикл идёт по
непрерывной строке правого операнда и пишет непрерывную строку результата,
указатели на строки вынесены из циклов, на свежевыделенный результат стоит
`restrict` (входы могут алиасить друг друга, поэтому на них его нет).
Пропуск нулей не добавлен сознательно: `0 * NaN` — это `NaN`, и «умная»
арифметика изменила бы семантику.

**Диагностики называют формы**, потому что с владеющим handle ошибка почти
всегда в размерах, а «shape mismatch» без чисел не говорит, какой операнд
виноват:

```
tensor: shape must be positive, got [2,0]
tensor: index out of bounds: [2,0] not inside [2,2]
tensor: elementwise: shape mismatch [2,2] vs [2,3]
tensor: matmul shape mismatch: [2,2] x [3,1]
tensor: matvec needs [2,3] x [3,1], got [2,3] x [2,1]
tensor: dot: shape mismatch [2,2] vs [4,1]
tensor: division by zero at [0,2]
tensor: clip needs low <= high
```

**Мелочи, которые ломали память:** `tensor_copy` проверяет handle до
разыменования `a->rows`, `tensor_zeros` отказывается от формы, при которой
`rows * cols * sizeof(double)` переполняет `size_t`, `tensor_free(NULL)` — no-op,
а `lu_tensor_elementwise` освобождает частичный результат перед `exit`, чтобы
не оставлять его на совести ОС.

**Тесты.** `make agent-test` — тест `tensor_ops` расширен до
copy/max/min/argmax/dot/abs/matvec/clip. `tests/cli_test.sh` собирает
v5-программу с `incl <tensor.h>` через `-c`, проверяет, что в выводе есть
`#include <tensor.h>` и нет реализации, сверяет stdout и код возврата, а
также требует диагностики для `Tensor` без `incl`, неверной арности,
нативного бэкенда и несовпадения форм в рантайме.

## v4.4 — библиотека `Uios`, настоящий `incl`, честный `||`

Появилась первая библиотека и вместе с ней — механизм, ради которого
библиотеки вообще нужны.

**`include/Uios.lu`** — потоки ввода-вывода в духе `<iostream>`: `Uout`
(fd 1), `Uerr` (fd 2), `Uin` (fd 0) и константа `Uendl`. Подключается через
`incl <Uios>` и целиком написана на v5 поверх `syscall`, поэтому одинаково
работает на нативном и на C-бэкенде.

- Сахар `<<` в транспайлере: по типу правого операнда выбирается
  `put_int` / `put_i64` / `put_byte` / `put_bool` / `put_str` / `put_ptr`.
  Методы возвращают `ptr/свой тип`, поэтому `Uout << a << b << Uendl` —
  одна цепочка, без временных переменных. Если слева от `<<` число, это
  по-прежнему сдвиг.
- Сахар `>>`: `Uin >> n` превращается в `get_int(&n)`. Поддерживаются
  `int`, `int64`, `byte`, `bool`; правый операнд обязан быть переменной.
- `get_line(&buf[0], cap)` читает строку до перевода строки, возвращает
  длину и дописывает `0`; `put_chars(p)` печатает такой буфер.
- Потоки без состояния: дескриптор зашит в тип, потому что `vid`-глобалы
  неизменяемы. Поле `pad: byte` держит структуру непустой — пустая
  структура это расширение C, а не стандарт.

**`incl` стал настоящим.** Раньше `incl` был декорацией: всё, что нужно
программе, и так лежало в ядре. Теперь путь ищется в каталоге включающего
файла, `.`, `include`, `../include`, рядом с бинарником и в `$LU_INCLUDE`,
файл вставляется потоком токенов до разбора тел (include-once — поэтому
цикл просто подключается один раз, а отсутствующий файл это ошибка со
строкой), а `std/*` остаётся встроенным no-op. `include/` рядом с `luc` —
штатное место для библиотек.

**Исправления:**

- Нативный бэкенд: `a || b` прыгал на ветку «ложь», поэтому `true || x`
  давал `false`, а `while c == 32 || c == 9` не выполнялся ни разу. Теперь
  `&&` и `||` используют отдельную метку выхода и дают 1/0 по смыслу
  оператора. Регрессия — `tests/native/logic.lu` (таблица истинности,
  короткое замыкание, цикл с `||`).
- C-бэкенд: объявления типов эмитятся до глобальных переменных, иначе
  глобал структурного типа (`vid Uout: Uout_t`) ссылался на ещё не
  объявленный `typedef`.
- Транспайлер: инструкция вида `IDENT .` (`Uout.put_str(...)` отдельной
  строкой) теперь начало инструкции, а не ошибка разбора.
- `free(): invalid pointer` при сплайсинге `incl` — база токенов
  освобождается один раз, после вставки.

**Тесты:** `make test-v5` — 22 проверки; каждая положительная программа
теперь собирается ещё и через C-бэкенд и сверяется по stdout и коду
возврата, а `.stdin` рядом с фикстурой подаётся программе на вход.
`tests/v5/good/13_uios.lu` покрывает оба потока, цепочки, `int64`, `byte`,
`bool`, `get_line` и `Uerr`; `tests/v5/good/12_incl.lu` впервые проверяет
реальный `incl` из подкаталога, а `tests/v5/bad/20_missing_incl.lu` — что
отсутствующая библиотека падает на компиляции, а не в рантайме.

## v4.3 — Lu v5

`luc` научился второму синтаксису. Файл с первой строкой `Lu/Strict`
компилируется целиком в v5-грамматике из `SYNTAX.md` и работает на обоих
бэкендах и в режиме `make`.

- Транспайлер `src/v5.c` переводит v5 в текст ядра v4, поэтому нативный
  бэкенд и C-бэкенд не пришлось менять: они по-прежнему получают знакомый
  вход. Ошибки указывают на строку исходника v5 — пустые строки
  добиваются, чтобы `path:LINE` совпадал.
- Двухпроходный разбор: сначала регистрируются сигнатуры `fn`/`struct`/
  `pack`, затем эмитятся тела, поэтому вызовы и методы не требуют
  предварительных объявлений.
- `pack byte` понижается до структуры с одним `int` и хелперами
  `__lu_pkget`/`__lu_pkset`; `bool`-поля читаются как `!= 0`, пишутся
  через `__lu_b2i`.
- Методы эмитятся как `Struct__method(self: ptr/Struct, ...)`;
  `h.m()` превращается в `Struct__m(&h)` или `Struct__m(p)`. Запись
  `set self.hp = ...` разрешена: `self` — указатель, меняется не привязка,
  а объект.
- Области видимости блочные: переменная `for` живёт только внутри цикла,
  поэтому два последовательных `for i in ...` — не затенение.
- Ошибки: запись в `vid`, затенение, отсутствие типа, неявное сужение
  `int -> byte` (`implicit conversion byte -> int; write int(...)`),
  несовпадение числа и типов аргументов. Все 8 негативных примеров из
  `SYNTAX.md` падают с диагностикой, 12 позитивных собираются.
- Регрессии: `tests/v5_test.sh` (`make test-v5`), 20 проверок, включая
  `make`-режим и оба бэкенда. `make test-all` теперь гоняет и их.

## v4.2 — режим `luc <file> make`

```bash
./luc hello.lu make      # собирает нативно, запускает, показывает вывод
```

- Второй позиционный аргумент `make` включает режим «собери и запусти»:
  бинарник пишется во временный файл в `$TMPDIR` (или `/tmp`), запускается
  через `fork`/`execv` с наследованием stdin/stdout/stderr, затем удаляется.
- Код возврата `luc` — код возврата программы (`return` из `main`); при
  гибели по сигналу — `128 + сигнал`, как в шелле.
- `-o prog` сохраняет бинарник на диске и запускает его оттуда.
- Временный путь резервируется через `open(O_CREAT|O_EXCL)` (а не
  `mkstemp`, который скрыт под `-std=c11`), поэтому пробелы и кириллица в
  именах файлов не мешают: `./luc "Привет мир.lu" make` работает.
- `make` несовместим с `-c` и с `-o *.c` — обе комбинации отклоняются с
  внятным сообщением.
- Регрессии: `tests/cli_test.sh` проверяет вывод, проброс кода возврата,
  отсутствие временных файлов и оба конфликта флагов.

## v4.1 — свой бэкенд x86-64 и длинный синтаксис

Цель релиза: убрать обязательную зависимость от gcc и сделать язык
низкоуровневее — при этом не потеряв C-бэкенд там, где он нужен.

### Главное: нативный бэкенд

`luc` теперь сам пишет исполняемый файл: **статический ELF64 для Linux
x86-64**, без ассемблера, линкера и libc.

```bash
./luc hello.lu              # → ./hello, статический ELF
./luc -c hello.lu -o hello.c  # → C для gcc, как в v4.0
```

- Новый модуль `src/x86.c` (+ `src/x86.h`, интерфейс `native_compile`):
  кодогенерация, таблица меток и правок, писатель ELF.
- Раскладка: заголовки + код с `0x401000` (сегмент `R|X`), данные на
  следующей странице (`R|W`), `PT_GNU_STACK`. Ни `PT_INTERP`, ни
  `DT_NEEDED` — программа не грузит ни одного `.so`.
- Точка входа `_start`: выставляет argc/argv, вызывает `__lu_top`
  (верхнеуровневые инициализации), затем `main`, затем `exit` с кодом
  возврата `main`.
- Выражения считаются в `rax`; левый операнд бинарной операции живёт во
  временном слоте кадра, правый идёт в `rcx`, сравнение — `cmp rax, rcx`.
- Два механизма правок адресов: `dfix`/`add_dfx` (disp32 в коде) и
  `gdfix`/`add_gdfx` (8-байтовый слот в `.data` с абсолютным адресом).
- Регистры параметров — SysV: `rdi, rsi, rdx, rcx, r8, r9`; для `syscall`
  четвёртый аргумент идёт в `r10`.

### Рантайм без libc

Прелюдия `LU_PRELUDE` в `x86.c` написана на самом Lu и компилируется тем же
парсером в каждую программу: `lu_strlen`, `lu_write`, `lu_puts`, `lu_putc`,
`lu_putn`, `lu_memset`, `lu_memcpy`, `lu_alloc`, `lu_free`. Отдельного
«магического» C-кода в бэкенде нет — только генерация.

### Встроенные `syscall`, `exit`, `strlen`

```lu
syscall(1, 1, "hi\n", 3)   // write
print(strlen("hello"))     // 5
exit(7)                    // код выхода 7
```

- В нативном бэкенде — прямые `syscall`; `strlen` считается своим циклом
  `lu_strlen`.
- В C-бэкенде — libc: `<unistd.h>`, `extern long syscall(long, ...)`,
  `exit`, `strlen` из `<string.h>`.
- `semantic.c: is_backend_builtin` знает эти три имени, поэтому они не
  считаются «undeclared function».

### Выбор бэкенда

| Как | Что получается |
|---|---|
| без флага | нативный ELF |
| `-n` | нативный ELF явно |
| `-c` | C-исходник |
| `-o file.c` | C-исходник (суффикс выбирает бэкенд) |

`-o` теперь необязателен: без него нативный бэкенд пишет файл с именем
исходника без расширения и ставит права `0755`.

### Длинные явные формы

Короткий синтаксис остался алиасом, добавлен полный стиль:

```lu
fn add(a: int, b: int) -> int { return a + b }

struct Point {
    x: int
    y: int
}

var p: Point
let n: int = 3
loop 4 { n = n + 1 }
raw { 0x90 }
```

- `fn name(...)` — то же, что `def name(...)`.
- `name: type` в параметрах функций и в полях структур.
- `var` / `let name: type = expr`.
- `loop <expr> { }` — ровно N итераций.
- `raw { 0x.. }` — сырые байты; в C-бэкенде `__asm__(".byte ...")`, в
  нативном — ошибка с отсылкой на `-c`.
- `var p: ptr/T` теперь разбирается как настоящее объявление указателя
  (`NODE_PTR_DECL`), а не как обычная переменная с типом `ptr/T`. Это убрало
  ложное предупреждение `Free/ on non-pointer` и выровняло поведение с
  формой `ptr/T p`. Массив указателей (`var a: ptr/T[4]`) остаётся обычным
  массивом.

### Код возврата `main`

`fn main() -> int` задаёт код выхода в обоих бэкендах. В C-бэкенде
пользовательский `main` переименовывается в `lu_user_main`
(`cg_rename_main`), а сгенерированный `main` возвращает
`(int)lu_user_main()`. `void`-main даёт 0.

### Исправления нативного бэкенда

- Сравнения: `cmp rax, rcx` даёт флаги для `lhs OP rhs` (перепутанный
  порядок операндов).
- `for x in arr`: условие выхода было `>` вместо `<=`.
- `print` для `byte[N]` и `ptr/byte` печатал адрес вместо строки.
- Лишний `lea` в `gen_print`.
- `var g_name: str` на верхнем уровне не получал `gdfix`.
- `loop N` не компилировался из-за `dec` по неверному регистру.
- `continue` в `loop N` и `for x in arr` зацикливал программу: введена
  отдельная метка `cont`, через которую счётчик всё равно
  инкрементируется/декрементируется.
- Составные имена (`b.n`, `b.data`, `a.b.c`) в `print`/`&`/выражениях:
  добавлен `resolve_dotted` рядом с `field_find`.
- Диагностика при неудачной записи выходного файла унифицирована:
  `[luc] failed to write output '<path>'`.

### Тесты

- `tests/native/*.lu` — программы паритета (`core`, `memory`, `loops`,
  `syscalls`). Каждая собирается `-n` и `-c`+`cc`; stdout и код возврата
  обязаны совпасть.
- `tests/native_test.sh` + `make test-native`: проверяет ELF-магию,
  отсутствие `INTERP`/`NEEDED` (если есть `readelf`) и паритет. Без
  C-компилятора нативная часть всё равно прогоняется (`[SKIP]`).
- `make test-all` теперь включает `test-cli` и `test-native`.
- `tests/cli_test.sh`: по умолчанию ожидается нативный ELF, отдельно
  проверяется `-c`.
- `src/native_demo.lu` — демонстрация возможностей нативного бэкенда.

### Ограничения

Нативный бэкенд не поддерживает `float`, `asm`, `Unique<T>`/`Vector<T>`,
многомерные массивы, X11 и Tensor; `for x in arr` — только по локальному
массиву. Для всего этого нужен `-c`.

## Core hardening — optional tensor library and removal of fake networking

- `Tensor` больше не является неявной частью языка. Для него обязательна точная
  директива `#include <tensor.h>`.
- Реализация вынесена из `codegen.c` в самостоятельную header-only библиотеку
  `include/tensor.h`; генерируемый C содержит только include и вызовы API.
- Компилятор проверяет наличие библиотеки, arity и базовые типы аргументов
  `tensor_*`. В рантайме проверяются размеры, формы, индексы, деление на ноль и
  ошибки выделения памяти.
- Удалены печатающие сетевые заглушки `Server`, `Snd`, `Rec`, `Bcast`, `Route`,
  `inq` и их C-рантайм. Их использование теперь является ошибкой компиляции.
- Публичный core остаётся сосредоточен на функциях, типах, управлении потоком,
  памяти и C-интеграции; предметные подсистемы должны жить в библиотеках.

## v4.0 — «Simple Core»: классы, указатели и asm без лишней сложности

Цель релиза: классы, указатели и прямой доступ к C там, где Lu отставал,
и упрощение типового кода — один вывод типов вместо трёх рассыпанных
эвристик, ошибки arity вместо молчаливого мусора.

### Главное: конструкторы с аргументами

Раньше `new Type(args)` ломал компиляцию: парсер терял возвращаемый тип
конструктора после `new(...)`, а codegen умел только примитивный
`new int(42)` (записывал первый аргумент через `*_t = (...)`). Теперь:

```lu
class Vec2 {
    int x
    int y
    new(int ax, int ay):void { this.x = ax; this.y = ay }
}
Vec2 a = new Vec2(1, 2)      // значение: Vec2 a; Vec2_init(&a, 1, 2);
ptr/Vec2 p = new Vec2(3, 4)  // указатель: calloc + Vec2_init(p, 3, 4)
auto d = new Vec2(5, 6)      // auto выводит Vec2*
```

- Конструктор — `new(params):void { }` или метод `Fn/init(params)`.
- `auto` корректно выводит класс и указательность.
- Работает и для структур с методами, и с наследованием (ctor базового
  класса находится через иерархию).

### Единый вывод типов (lu_type_of)

Вместо трёх рассыпанных эвристик (`expr_kind_from_ast`, `infer_c_type`,
`infer_type` в semantic) — одна функция `lu_type_of(cg, node, &is_ptr)`,
вычисляющая Lu-тип выражения из таблицы символов и реестра классов:
поля классов (`p.x`), методы (`p.mag()`), перегрузки операторов
(`auto c = a + b` → класс результата!), `new`, векторные `get/pop`,
функциональные переменные, тернарники. Старые эвристики сохранены как
фолбэк (`expr_kind_legacy`) — ничего из того, что работало, не сломалось.

### Проверка arity — ошибки компиляции, не segfault'ы

```lu
def add(int a, int b) -> int { return a + b }
add(1, 2, 3)
// [LU ERROR] line 5: 'add' expects 2 argument(s), got 3
```

Проверяются вызовы пользовательских функций, методы классов и
конструкторы (`new Vec2(1)` → error). exit-код 1.

### Низкий уровень: sizeof, const, вложенные инициализаторы

```lu
print(sizeof(int))            // 4 — тип или выражение
print(sizeof(arr))            // размер массива
const int K = 7               // C-константность для локальных и глобальных
int m[2][2] = {{1, 2}, {3, 4}}  // вложенные скобки — раньше не парсились
```

### Проще: print с несколькими аргументами

```lu
print(1, 2.5, "three", True)   // → 1 2.5 three true
```

Раньше лишние аргументы молча выбрасывались.

### Прочее

- `luc -v` → 4.0.
- Уточнение документации: флаг `-O0..-O3` записывает уровень в комментарий
  сгенерированного C; оптимизирует сам gcc (флаг `-O` при сборке программы).

### Попутные исправления компилятора (найдены при написании GUI-примера)

Пример `src/calculator_gui.lu` — калькулятор с X11-графикой, написанный
целиком на Lu (`Import "<X11/Xlib.h>"` + прямые вызовы C). Его написание
вскрыло и починило пять дефектов компилятора:

- **`return` перед `}` в однострочном теле** — `if x { return }` пытался
  разобрать `}` как выражение (ошибка семантики). Теперь bare-`return`
  распознаётся перед любым закрывающим токеном.
- **Многострочные вложенные инициализаторы** — `int F[2][2] = {\n{1,2},\n{3,4}\n}`
  не парсились (переводы строк внутри `{...}`); глобальные 2D-массивы
  с переносами теперь работают.
- **`Import "<X11/Xlib.h>"`** — угловой путь в кавычках эмитился как
  `#include "<...>"` (несуществующий файл); теперь это системный include.
- **Мягкий режим семантики при C-заголовках** — если в программе есть
  `Import "<..."`/`"file.h"`, вызовы неизвестных функций и константы
  заголовков (XOpenDisplay, Expose, XK_*) — предупреждения, а не ошибки.
  В чистых Lu-программах опечатки по-прежнему останавливают компиляцию.
- **`ptr/T` глобальные переменные** — эмитились после функций (неявные
  объявления в C); теперь попадают в преамбулу глобалей.
- **`print` с unresolved-типом** — печатал через `printf("%s", …)` любой
  тип (segfault на long); для «auto» теперь безопасный `lu_print`
  с `_Generic` по фактическому C-типу.

### Регрессия

4 новых кейса доктора: `constructors`, `op_overload_auto`,
`sizeof_const`, `print_multiarg`. Гейт: `make clean && make && make test &&
make test-all && make agent-test && make test-gui && ./bootstrap.sh &&
./fuzz_test.sh` — всё зелёное (65/65, 0 предупреждений, фаззинг 100/100).

### Не входит (осознанно)

- Виртуальные вызовы/vtable — `virtual` по-прежнему маркер без диспетчеризации.
- Потоки под Spawn/Chan — примитивы остались однопоточными.
- Мономорфизация `template<T>` — по-прежнему void*.

## v3.3 — OOP/stdlib consistency pass

Probe pass over every README-documented feature found five places where the
documented surface didn't match actual compiler behavior. All fixed, six new
doctor cases.

### Bugs fixed

- **`def` methods inside class bodies generated invalid C** — the class parser
  only recognised `Fn/` methods, so `def inc(int by) -> void { }` fell into the
  field-parsing branch and emitted garbage like `def inc;` into the C struct.
  Methods declared with `def` now parse via the same path as top-level `def`
  functions (extracted into `parse_def_func`) and are lowered exactly like
  `Fn/` methods, including `virtual`/`override` prefixes.
- **`Vector<str>` / `Vector<float>` / any non-int element type didn't compile**
  — codegen emitted `lu_vector_str`/`lu_vector_float` calls, but the runtime
  only ever defined `lu_vector_int`. The vector runtime is now emitted
  monomorphically per element type actually used in the program (int, float,
  str, bool, byte, int64, user struct/class names). Programs that use no
  vectors get no vector runtime, mirroring the v3.2 tensor approach.
  f-string type inference for `v.get(i)` / `v.pop()` now follows the element
  type (was hardcoded to int — would have misprinted `Vector<str>` results).
- **Single-quoted strings were not lexed** — `str s = 'hello'` was a syntax
  error, and there was no way to nest quotes inside an f-string, so
  `f"val={x > 5 ? 'big' : 'small'}"` failed. `'...'` and `f'...'` now work,
  with `\'` escape.
- **`Recv/ch` received a value and threw it away** — as a statement the codegen
  emitted `lu_chan_recv(ch);` discarding the result, and it couldn't be used as
  an expression at all (`print(Recv/ch)` failed semantic analysis). `Recv/ch`
  now works in expression position (values travel as `(void*)(intptr_t)`, cast
  back on receive); the bare statement form still works.
- **Struct bodies silently swallowed methods** — `Fn/push(int v):void { }`
  inside a `struct` was misparsed as a field plus garbage. Structs now support
  methods (`Fn/` and `def` forms), lowered like class methods
  (`Struct_method(Struct *this, ...)`). Plain method-less structs keep their
  previous simple emission.
- **`luc -v` reported v3.0** while README/PATCH_NOTES were at v3.2 — version
  bumped to 3.3.

### Regression coverage

6 new cases in the doctor suite: `def_methods_in_class`, `struct_methods`,
`vector_str`, `vector_float`, `single_quoted_strings`, `recv_expression`.
Gate: `make clean && make && make test && make test-all && make agent-test &&
./bootstrap.sh && ./fuzz_test.sh` — all green (61/61 doctor cases, fuzz
100 rounds: 0 crashes / 0 timeouts).

### Still known limitations (unchanged)

- `Vector<T>` with nested/pointer elements (`Vector<Vector<int>>`,
  `Vector<int*>`) is unsupported — collection skips them, so they fail the
  same way as before rather than silently changing.
- Vector auto-init still requires the declaration to have no initialiser.

## v3.2 — Bug fixes + logic words + inline asm + Tensor stdlib

### Bugs fixed

- **`def main()` broke codegen** — user-defined `main` collided with the generated
  `int main(argc, argv)` entry point (`conflicting types for 'main'`). Now renamed to
  `lu_user_main` and called first from the real entry point.
- **f-string `{{`/`}}` escapes** — `f"{{literal}}"` previously swallowed `{{}}` as an
  empty expression and produced a confusing semantic error (`use of undeclared identifier '{'`).
  Brace escapes now work.
- **String `==` compared pointers, not contents** — `"a"+"b" == "ab"` was pointer
  comparison (accidentally true for literals due to compiler dedup, false at runtime).
  Lowered to `strcmp(...) == 0`. Strict guards keep pointer comparisons intact:
  `fp == 0`, `p == null`, and any comparison without a provably string-like operand.
  (First attempt lowered `fp == 0` too — segfaulted the self-hosting compiler; caught by bootstrap.)
- **Inline boolean expressions printed as 0/1** — C comparisons yield `int`, so
  `_Generic` in `lu_print` missed the `bool` case. Comparisons/`!` are now wrapped
  in `(bool)(...)`; `print(1 == 1)` prints `true`.
- **`bootstrap.sh` / `fuzz_test.sh` not executable** in the distributed archive.

### New features

- **Logic words**: `and` / `or` / `not` (lowered to `&&` / `||` / `!`),
  capitalized literals `True` / `False` / `None`.
- **`in` operator**: `"ell" in "hello"` → `lu_str_contains(haystack, needle)`,
  precedence equal to comparisons.
- **`asm { ... }` block**: GCC basic asm, AT&T syntax. Raw source text is preserved
  verbatim via token line/col slicing (the lexer shreds operands like `$2` and `%rax`
  into separate tokens). Lines joined with `\n\t` into one `__asm__ volatile("...")`.
  If a program contains any asm block, file-scope variables are emitted `volatile`
  so the optimizer cannot constant-fold reads that asm mutates.
- **Tensor stdlib**: `tensor_zeros/ones/full/eye`, elementwise
  `add/sub/mul/div`, `matmul`, `scale`, `transpose`, `relu`, `sigmoid`, reductions
  `sum/mean`, `get/set/fill/rows/cols/print/free`. Row-major double matrices.
  Runtime code is emitted into generated C only when the program actually uses
  `tensor_*` or declares `Tensor` — no libm dependency is added otherwise.

### Regression coverage

6 new cases in the doctor suite: `fstring_escape`, `def_main_entry`,
`logic_words`, `str_eq_strcmp`, `asm_block`, `tensor_ops`.
Gate: `make clean && make && make test && make test-all && make agent-test &&
./bootstrap.sh && ./fuzz_test.sh` — all green (55/55 doctor cases).

## v3.1 — Full technical audit + On//Spawn fix + Server/Route/Bcast deprecation

### Technical audit (80 steps)

Full codebase audit covering lexer, parser, semantic, codegen, and runtime. 170 fuzz tests run. 7 bugs found and fixed, 8 non-critical issues documented.

**Bugs fixed:**
- `cur()` in parser had no bounds check — OOB read on error recovery
- `peek_tok()` didn't check negative index — OOB read
- `cg_class_resolve_method` infinite loop on cyclic inheritance (A→B→A)
- `emit_c_ident` had 2 inheritance walks without cycle detection
- `new Type(value)` used Lu type names instead of C types (`str` → `str*` instead of `char*`)
- `auto` type inference registered C types (`double`) instead of Lu types (`float`) — f-strings broke on `auto y = 3.14`
- Generated C had 180 unused-function warnings — suppressed via `#pragma GCC diagnostic`

**Exit code fix (from v3.0.1):**
- `g_parse_error_count` global counter — `main()` returns exit 1 if > 0
- Checked after parse, semantic, AND codegen phases
- `luc broken.lu` now correctly returns exit 1 (was 0)

### On/ and Spawn/ — working but not strictly type-safe

Previously these were "known broken" — they generated invalid C that segfaulted. Now:

- **`On/name handler`** — `handler` must be a function name (identifier), not a string. Generates `lu_event_on(ev_name, (void(*)(void*))handler)`. String literals are rejected with a clear error.
- **`Spawn/function`** — `function` must be a function name. Generates `lu_spawn((void(*)(void))function)`. Non-identifier arguments are rejected.

Both produce C that compiles without warnings under `-Wall -Wextra -Wpedantic` and work at runtime. However, the function-pointer cast is **technically undefined behavior** in C if the user's handler signature doesn't exactly match `void(*)(void*)` or `void(*)(void)`. This works on x86-64 because all pointer types share the same representation, but is not portable to platforms where calling conventions differ. The casts suppress the compiler warning; the behavior is correct in practice on the target platform.

### Fuzz testing — reproducible

A fuzz test script (`fuzz_test.sh`) is now included in the repository. It generates random programs and verifies `luc` doesn't crash (segfault) or hang (timeout). Run with:

```
make fuzz           # 100 rounds (default)
make fuzz ROUNDS=500
```

This is a **manual test**, not part of `make agent-test`. It checks crash/timeout resistance only — not correctness of generated C (that's `agent-test`'s job). The 170-round fuzz mentioned in the audit was a one-time manual run; the script in the repo makes this reproducible.

### Server/Route/Bcast — formally deprecated

These v1.x network primitives were marked "experimental, untested" in v2.1.2. After testing during the audit, they were found to generate C that compiles but has no real network functionality (just `printf` stubs). They are now formally **deprecated**:

- `Server #qN cor/{...}` — generates `lu_server_new()` stub, no actual server
- `Route/ dst via gateway` — generates `lu_route()` stub, no actual routing
- `Bcast{payload}` — generates `lu_broadcast()` stub, no actual broadcast
- `Snd{payload}` — generates `lu_send()` stub, no actual networking

This deprecated compatibility layer was removed in the core-hardening pass
documented above; these constructs now produce a compilation error.

### Build quality

- Compiler: **0 warnings** with `-Wall -Wextra -Wpedantic`
- Generated C: **0 warnings** for simple programs (3 for `example.lu` — unused consts, expected)
- `make agent-test`: **47/47** pass
- `make test-all`: all bundled programs compile and run
- `./bootstrap.sh`: self-hosting verified
- 170 fuzz tests: **0 crashes, 0 timeouts**

---

## v3.0 — Массивы, указатели, классы и управление ресурсами

### Added in 7 iterations

**Базовые конструкции:**
- Multi-dimensional arrays `int m[3][3]`, `int cube[2][2][2]`
- Type casting `(int)3.7`, `(byte)x`
- printf/fprintf with full format specifiers
- Pointer arithmetic `p + 1`, `p += 1`, `p - arr`
- Function pointers `fn(int, int) -> int op = add`
- `union` type

**Объекты и классы:**
- Inheritance `class Dog extends Animal` with proper struct layout
- Inherited methods and fields (walks class hierarchy)
- `super.method()` for parent method calls
- Operator overloading `op+`, `op==` — `a + b` → `Vec2_op_add(&a, b)`
- Lambdas `fn(int a, int b) -> int { return a + b }`

**Ресурсы, опциональные типы и срезы:**
- `defer` — scope-exit cleanup via GCC nested functions + cleanup attribute
- Optionals `?T` — `?int maybe = null` → `int* maybe = NULL`
- `null` literal → `NULL`
- Error unions `!T` — simplified (lowers to T, errors via Try/Catch)
- Slices `arr[1:4]` → `lu_slice_t { ptr, len }`
- Tagged unions via `enum + union + struct` (existing primitives)

**Quality:**
- Error recovery in parser — warn + continue instead of exit(1) on first error
- 17 new regression tests (total: 47)

---

## v2.1.2 — Chan/ parser fix + documented hidden layer

### Critical bug fix (invalid C generation)

`Chan/mychan` generated syntactically invalid C — the variable name was lost:

```c
lu_chan_t * = lu_chan_new();   // ← missing identifier
```

**Root cause:** `parse_statement` for `TOK_CHAN` called `consume_type(p)` to read the channel's "type", but `is_type_tok` includes `TOK_IDENT`, so it swallowed the channel name (`mychan`) as the type. The subsequent `n->sval = cur(p)->value` then grabbed whatever token came next.

**Fix:** Channels are always `lu_chan_t*` — there is no type to parse. The parser now hardcodes the type and reads the name directly:

```c
case TOK_CHAN: {
    consume(p);                          /* eat Chan/ */
    ASTNode *n = node_new(NODE_CHAN_DECL, line);
    n->type_name = lu_strdup("chan");
    n->sval = lu_strdup(cur(p)->value);  /* the channel name */
    consume(p);
    return n;
}
```

The codegen symbol table also now registers the channel as a known pointer, so later references don't warn.

### Bonus fix: `chan <- value` now parses at statement level

The `<-` operator was only recognised inside the legacy `Send/chan <- value` form. The README documents `chan <- value` directly, but the parser treated it as a stray expression statement. Now `mychan <- 42` lowers to `lu_chan_send_safe(mychan, ...)` correctly.

### Documentation: the hidden layer

Lu has a large undocumented surface — network primitives, async, events, debug, and a "game engine" DSL. These were implemented in v1.x but never advertised. This release adds a **"Network, async, debug"** section to README and TUTORIAL covering what actually works:

**Working (tested):**
- `Chan/name` + `name <- value` — channels
- `Event/name` + `Emit/name data` — events
- `Log/level msg` — leveled logging
- `Assert/cond msg` — assertions
- `Try/ { } Catch/ERR_X { } Finally/ { }` — exceptions
- `Throw/ERR_MEM "msg"` — throw

**Known broken (documented as unsupported):**
- `On/name callback` — passes a string where a function pointer is expected → segfault on emit
- `Spawn/expr` — passes an int where a function pointer is expected → segfault on call

These two are now explicitly listed as **not supported** in README rather than silently generating bad C.

### Validation

- New regression tests: `channel_send`, `log_levels`, `event_emit`
- All **30/30** agent tests pass (was 27)
- `make`, `make test`, `make test-all`, `./bootstrap.sh` all pass
- The `messenger.lu` demo still compiles with 0 warnings

---

## v2.1.1 — segfault fix: array indexing in f-strings

### Critical bug fix (segfault)

`f"{arr[i + 1]}"` on an `int` array caused a **segfault**. The generated code was `printf("%s\n", arr[(i + 1)])` — treating the `int` value as a `char*` pointer.

**Root cause:** `expr_kind_from_ast` (the type inference used by f-string codegen) had no `case NODE_EXPR_INDEX` for array indexing. It fell through to `default: return "auto"`, and `"auto"` means "emit as-is assuming char*". So `arr[i+1]` (an int) was passed directly to `lu_str_concat`/`printf` as a `char*`.

**Fix:** Added `NODE_EXPR_INDEX` to `expr_kind_from_ast`. It now:
1. Looks up the base array identifier's registered type (which is the element type)
2. Returns the appropriate kind ("int", "float", "bool", "str")
3. Falls back to "int" as a safe default (int→str conversion via `lu_str_from_int` is always safe — no segfault possible)

This also fixes the same class of bug for these previously-unhandled node types in `expr_kind_from_ast`:
- `NODE_EXPR_INDEX` — array indexing `arr[i]`
- `NODE_EXPR_TERNARY` — ternary `cond ? a : b` (infers from "then" branch)
- `NODE_NEW_EXPR` — `new Type()` (treated as pointer/str)
- `NODE_EXPR_DEREF` — `*p` (treated as int)
- `NODE_EXPR_REF` — `&x` (treated as pointer/str)

### Validation

- New regression tests: `fstring_array_index`, `fstring_array_float`
- All **27/27** agent tests pass
- `make`, `make test`, `make test-all`, `./bootstrap.sh` all pass
- The `messenger.lu` demo still compiles with 0 warnings

---

## v2.1 — f-strings v2 + match/case

This release replaces the string-based f-string implementation with a real AST-based parser, and adds `match/case`.

### F-strings v2 (real expression parsing)

The previous f-string implementation used text-based heuristics to determine the type of each `{...}` expression. This worked for simple cases but broke on complex expressions like `f"{obj.method()}"`, `f"{arr[i + 1]}"`, or `f"{p.x}"`.

**v2 fixes this properly:**
- The expression inside `{...}` is now parsed by a **temporary Lexer + Parser** into a real AST node.
- Type inference uses the same `expr_kind_from_ast` logic as the rest of the compiler — no more string heuristics.
- All of these now work:
  ```lu
  f"{p.x}"                    // field access
  f"{obj.method()}"           // method calls
  f"{square(square(3))}"      // nested calls
  f"{x > 40}"                 // comparisons (→ bool)
  f"{1 + 2 * 3}"              // arithmetic
  f"{len(s)}"                 // built-in functions
  ```
- New AST node `NODE_FSTRING` holds the parsed parts (literal text + expression ASTs).
- f-strings now participate correctly in `auto` inference: `auto s = f"x={x}"` → `char*`.
- f-strings work in string concatenation: `f"a={a}" + f"b={b}"`.

### match/case

New `match`/`case`/`default` construct, lowered to an `if/else if` chain:

```lu
match code {
    case 200 { print("OK") }
    case 404 { print("Not Found") }
    case 500 { print("Server Error") }
    case _   { print("Unknown") }
}
```

- `case _` is the default case.
- The matched expression is evaluated once into a temp variable.
- Cases are compared with `==`.
- Works with any integer-like expression.

### Bug fixes from v2.0

- **Global variables**: top-level `NODE_VAR_DECL` is now emitted as `static` before functions, so functions can reference globals (previously globals were emitted inside `main()` and functions couldn't see them).
- **List literal element types**: `[strings]` now generates `char*[]` instead of always `int[]`. The element type is inferred from the first element.
- **`for x in range(n)` inside f-strings**: the loop variable is now registered in the codegen symbol table, so f-strings inside the loop body know its type.
- **`obj.field` type inference in f-strings**: `main_chat.is_group` no longer falsely detected as float (the `.` in `obj.field` was confused with a float literal `3.14`).

### Documentation

- **TUTORIAL.md**: new step-by-step tutorial from Hello World to a full messenger demo.

### Validation

- `make`, `make test`, `make test-all`, `make agent-test` (25/25), `./bootstrap.sh` all pass.
- New regression cases: `fstring_v2_complex`, `match_case`, `match_default`, `fstring_arithmetic`.
- The `messenger.lu` demo now compiles with **0 warnings** (was 12 in v2.0).

---

## v2.0 — Компактный синтаксис и возможности ООП

This build is still a small young compiler, but the core is now much more usable for writing real Lu programs.

## v1.3 — bug-fix pass

This release fixes several parser/codegen bugs that could hang the compiler or produce invalid C, and adds two new features.

### Compiler correctness

- **Struct/class with array fields no longer hangs the parser.** The previous build went into an infinite loop on `struct Foo { int arr[5] }` because the `[5]` after the field name was never consumed. Both `parse_struct` and `parse_class` now accept `type name[size]` and `type name[size] = {...}` syntax, and the codegen emits the size into the C struct definition.
- **`Set/this.field[idx] = v` with a multi-token index now parses correctly.** The previous Set/ parser only took a single token as the bracket index, so `Set/this.data[this.top] = v` silently truncated to `this.data[this]`. The parser now consumes the full bracketed expression (with proper depth tracking for nested brackets) and the codegen lowers embedded `this.X` references correctly.
- **`emit_c_ident` now translates `this.`/`super.` anywhere in the lvalue string**, not just at the start. This fixes codegen for `Set/this.data[this.top] = v` and similar expressions where the embedded `this.top` would otherwise leak into the C output as `this.top` (invalid because `this` is a pointer).
- **Compound assignment operators are now supported.** All ten C-style compound forms work: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`. They work on plain variables, struct fields, class fields, and array indices. `x += y` lowers to `x = (x + y)`.

### Type-checker improvements

- **Enum aliases no longer produce false "type mismatch" warnings.** `Color c = RED` now type-checks because enum members are registered with their enum type instead of `int`. Enum↔int is also accepted in either direction.
- **`Loop/Each x in arr { ... }` no longer warns about `x` being undeclared.** The loop variable is now added to the symbol table before the body is checked.

### New features

- **String concatenation with `+`.** When either operand of `+` is a string literal, a `str`-typed variable, or another string concatenation, the compiler emits a `lu_str_concat(a, b)` call instead of invalid `char* + char*` C code. The runtime helper returns a freshly `malloc`'d string. Non-string operands (int, float, bool) are automatically wrapped in `lu_str_from_int` / `lu_str_from_float` / `lu_str_from_bool` so users can write `"Count: " + 42` directly. Examples:
  ```lu
  str greeting = "Hello, " + name + "!"
  str multi = "a" + "b" + "c"
  str with_int = "Count: " + 42
  ```

### Template class methods

- Methods of `template<T>` classes now lower `T` to `void*` in their signatures too (previously only fields were lowered). Calling these methods with non-pointer arguments requires manual casts; full monomorphization is not implemented.

### Version

- Bumped to `1.3` to match README and `luc -v`.

### Validation

- `make`, `make test`, `make test-all`, `make agent-test`, `./bootstrap.sh` all pass.
- New manual test: `struct`/`class` with array fields, `Set/this.field[idx] = v`, all compound operators, enum aliases, string concatenation, and `Loop/Each` body access.

## v1.2 — earlier hardening pass

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

### Низкоуровневая реализация ООП

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
