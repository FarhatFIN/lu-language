<p align="center">
  <img src="assets/logo.png" alt="Lu" width="180">
</p>

# luc — Lu Language Compiler v4.8

**Lu** — компилируемый язык программирования с **компактным синтаксисом**, **явной статической типизацией** и **низкоуровневой интеграцией с C**.
Два бэкенда: **собственный x86-64** (по умолчанию, без gcc) и **C11** (для GUI, X11 и тензоров).
Компилятор написан на C.

```
Lu/Language  →  luc -n  →  static x86-64 ELF        (syscalls only, no libc)
Lu/Language  →  luc -c  →  output.c  →  gcc  →  ./program
```

Основное ядро языка — функции, структуры и классы, циклы, массивы, указатели,
обработка ошибок и C FFI (в Lu v5 FFI пока нет — см. `SPEC.md` §11). Необязательные
подсистемы подключаются заголовками, а не встраиваются в каждую программу.
Старые сетевые команды `Server`, `Snd`, `Rec`, `Bcast`, `Route` и `inq` удалены:
они были печатающими заглушками, а не сетевой реализацией.

---

## Какой синтаксис выбрать

- **Lu v5** — канонический язык. Новый код пишите на нём; файл начинается
  строкой `Lu/Strict` (это маркер формата, а не название языка).
- **Lu Classic** — замороженный синтаксис v4.x и старше. Компилируется, но
  новые возможности в него не добавляются: см. `docs/meta/classic-freeze.md`.
- Различия и перенос — раздел «Deltas from Classic» в `SPEC.md`; справка по
  конструкциям — `SYNTAX.md`.

---

## Что нового в v4.6 — `incl` в v4 и приложение LuMotion

**`incl` работает в основном синтаксисе, а не только в v5.** `incl <core.lu>`
вставляет исходник на Lu в программу до лексинга, `incl <X11/Xlib.h>`
превращается в `#include`. Include-once сравнивает пути после `realpath`,
поэтому «ромб» из двух библиотек не даёт повторных определений, а цикл
`a → b → a` просто останавливается. Именно это позволило разложить
приложение на библиотеку и два фронтенда.

**Появилось приложение: [`motion/`](motion/README.md) — LuMotion.** Редактор
моушн-графики на X11: слои (прямоугольник, эллипс, текст, градиент,
свечение), кейфреймы с тремя режимами ease, четыре blend-режима, таймлайн,
инспектор, превью в реальном времени и headless-рендерер в PPM. Около 2300
строк на Lu, без SDL, OpenGL и шрифтовых библиотек: растеризация
аналитическая, шрифт 5×7 встроен в ядро. Формат проекта — текстовый `.lum`,
он же читается скриптами. Проверки: `motion/test.sh` (или `make -C src
test-motion`).

**`else if` исправлен.** Цепочка компилировалась в два независимых `if`, и
`else` привязывался ко второму из них — теперь это обычное вложение.

Подробности по версиям — [PATCH_NOTES.md](PATCH_NOTES.md); про `incl`
в v4 — [GUIDE.md](GUIDE.md#включаемые-файлы-incl).

---

## Что нового в v4.4 — `Uios`, потоки ввода-вывода

Первая настоящая библиотека: [`include/Uios.lu`](include/Uios.lu). Подключается
одной строкой `incl <Uios>` и даёт три готовых потока в духе `<iostream>`:

| Поток | fd | Пишет | Читает |
|---|---|---|---|
| `Uout` | 1 (stdout) | `<<`, `put_str/int/i64/byte/bool/ptr/bytes/chars` | — |
| `Uerr` | 2 (stderr) | `<<`, те же `put_*` | — |
| `Uin`  | 0 (stdin)  | — | `>>`, `get_line` |

```lu
Lu/Strict

incl <Uios>

fn main() -> int {
    Uout << "Как тебя зовут? " << Uendl

    var name: byte[64]
    var n: int = Uin.get_line(&name[0], 64)

    var age: int = 0
    Uin >> age

    Uout << "Привет, " << n << " байт по имени "
    Uout.put_chars(&name[0])
    Uout << ", " << age << " лет" << Uendl
    ret 0
}
```

- `<<` — сахар транспайлера, а не магия бэкенда: по типу правого операнда
  выбирается метод (`int` → `put_int`, `int64` → `put_i64`, `byte` →
  `put_byte`, `bool` → `put_bool`, `str` → `put_str`, `ptr/T` → `put_ptr`).
  Каждый метод возвращает `ptr/Uout_t`, поэтому цепочка собирается сама.
- `>>` так же превращается в `get_int` / `get_i64` / `get_byte` / `get_bool`
  и требует переменную справа — писать некуда больше нечему.
- Вывод небуферизован: один `<<` — один `write(2)`. Поэтому `Uendl` — просто
  `vid Uendl: str = "\n"`, а `flush()` существует для привычки из C++.
- Библиотека написана только через `syscall`, поэтому одинаково работает на
  **обоих** бэкендах: нативный ELF и C-код выводят байт в байт одно и то же.

`incl` стал настоящим: путь ищется в каталоге включающего файла, затем в `.`,
`include`, `../include`, рядом с бинарником `luc` и в `$LU_INCLUDE`; файл
вставляется как поток токенов, повторное подключение игнорируется (поэтому
цикл не зацикливает сборку), отсутствующий файл — ошибка компиляции со
строкой `incl`. `incl <std/io>` остаётся no-op для совместимости.

Заодно исправлен баг нативного бэкенда: `a || b` перепрыгивал на ветку «ложь»,
поэтому `true || x` давал `false`. Теперь `&&` и `||` считаются честно и
проверяются регрессией `tests/native/logic.lu`.

---

## Что нового в v4.3 — Lu v5

`luc` понимает второй, канонический синтаксис Lu v5 из [`SYNTAX.md`](SYNTAX.md). Файл,
первая строка которого — `Lu/Strict`, разбирается как программа v5 и
транслируется во внутреннее ядро v4, поэтому работают **оба бэкенда** и
режим `make`:

```bash
./luc tests/v5/good/01_arith.lu make      # нативный ELF + запуск
./luc tests/v5/good/01_arith.lu -o out.c  # C-бэкенд
```

```lu
Lu/Strict

incl <std/io>

vid LIMIT: int = 100

struct Hero {
    pub name: str
    pub hp: int
    secret: int

    fn alive(self) -> bool {
        ret self.hp > 0
    }
}

fn main() -> int {
    var h: Hero
    set h.name = "Lu"
    set h.hp = 100
    if h.alive() {
        Pr(h.name, h.hp)
    }
    ret 0
}
```

Чем v5 отличается от v4: `vid` (неизменяемая привязка) против `var`,
единственная форма присваивания — `set`, обязательный тип в объявлении,
`ptr/T` и `T[N]`, битовые поля через `pack byte Name { vid0..1 x: bool }`,
`fn`/`struct`/`self`, `loop`/`for i in a..b`/`match`, `Del x` вместо
`Free`, `Pr`/`PrBits`/`dv(&x)` вместо `print`/`scan`, `incl <std/io>`
вместо `#include`. Ни одного `;`.

Диагностика привязана к строкам исходника v5 (`path:LINE`), а не к
промежуточному тексту. Затенение, запись в `vid`, неявное сужение
`int -> byte` и несовпадение типов аргументов — ошибки компиляции.
Эталонный набор из `SYNTAX.md` (12 примеров + 8 нарушений) проверяется
командой `make test-v5`.

## Что нового в v4.2 — `luc file.lu make`

Сборка и запуск одной командой:

```bash
./luc hello.lu make
```

Нативный бэкенд собирает ELF во временный файл в `$TMPDIR` (или `/tmp`),
`luc` запускает его с наследованием stdin/stdout/stderr и показывает вывод
программы прямо в терминале. Код возврата `luc` — это код возврата самой
программы, поэтому `./luc hello.lu make; echo $?` честно повторяет `return`
из `main`. Временный бинарник удаляется; с `-o prog` он остаётся на диске.
С `-c` режим несовместим — ему нужен именно нативный бэкенд.

## Что нового в v4.1 — свой бэкенд x86-64

Компилятор больше не обязан звать `gcc`. По умолчанию `luc` пишет
**статический ELF64** для Linux x86-64 сам: заголовки, таблицы программ,
машинный код и данные. Ни ассемблера, ни линкера, ни libc — только `syscall`.

```bash
./luc hello.lu                  # → ./hello  (статический ELF, ~4 КБ)
./luc hello.lu make             # → собирает и сразу запускает, вывод в терминале
./luc -c hello.lu -o hello.c    # → C-бэкенд для gcc, как в v4.0
```

Что покрывает нативный бэкенд: функции и рекурсию, глобалы, `if`/`else`,
`while`, `loop N`, `for x in arr`, `break`/`continue`, структуры и их поля,
массивы (одномерные), `ptr/T`, `&`/`*`, `Alloc/`, `Free/`, `Memset/`,
`Memcpy/`, `print`, строки и `byte[N]`, встроенные `syscall` и `exit`.
Чего нет: `float`, `asm`, шаблоны (`Unique<T>`, `Vector<T>`), многомерные
массивы, X11 и тензоры — для них нужен `-c`.

Встроенный рантайм (свой, не libc) — `lu_strlen`, `lu_write`, `lu_puts`,
`lu_putc`, `lu_putn`, `lu_memset`, `lu_memcpy`, `lu_alloc`, `lu_free`. Он
компилируется тем же парсером из строки-прелюдии, так что бэкенд не содержит
отдельного «магического» кода.

### Длинные явные формы

Короткие формы остались алиасами, но появился полный, «низкоуровневый» стиль:

```lu
fn add(a: int, b: int) -> int {     // было: def add(int a, int b) -> int
    return a + b
}

struct Point {
    x: int                          // поля через `:`
    y: int
}

fn main() -> int {
    var p: Point
    let n: int = 3
    loop 4 {                        // ровно 4 итерации
        n = n + 1
    }
    ptr/byte buf = Alloc/byte:16
    raw { 0x90 }                    // сырые байты (только -c)
    Free/buf
    return 0
}
```

### Код возврата `main`

`fn main() -> int` теперь задаёт код выхода процесса в **обоих** бэкендах:
нативный возвращает его из `_start`, C-бэкенд оборачивает пользовательский
`main` в `lu_user_main` и возвращает результат из настоящего `main`.

### Код возврата, `syscall` и `strlen`

```lu
syscall(1, 1, "hi\n", 3)   // write(1, "hi\n", 3) — напрямую в ядро
exit(7)                    // код выхода 7
print(strlen("hello"))     // 5 — свой цикл в нативном бэкенде, libc в -c
```

`syscall` и `exit` в C-бэкенде опускаются до libc, поэтому программа
собирается и работает одинаково.

---

## Что нового в v4.0 — «Simple Core»


Классы, указатели и asm — без усложнения синтаксиса.

### Конструкторы с аргументами

```lu
class Vec2 {
    int x
    int y
    new(int ax, int ay):void {
        this.x = ax
        this.y = ay
    }
    Fn/mag():int { Ret/this.x * this.x + this.y * this.y }
}

Vec2 a = new Vec2(1, 2)       // значение на стеке
ptr/Vec2 p = new Vec2(3, 4)   // в куче
auto d = new Vec2(5, 6)       // auto выводит Vec2*
print(a.mag())                // 5
```

Вместо `new(...)` можно определять метод `Fn/init(params)` — работает одинаково.

### Операторы классов + auto

```lu
op+(Vec2 other):Vec2 {
    Vec2 r = new Vec2(this.x + other.x, this.y + other.y)
    return r
}
auto c = a + b      // тип результата — класс, выведенный из op+
```

### Ошибки arity вместо молчаливого мусора

```lu
def add(int a, int b) -> int { return a + b }
add(1, 2, 3)
// [LU ERROR] line: 'add' expects 2 argument(s), got 3
```

Проверяются функции, методы и конструкторы.

### Низкий уровень

```lu
print(sizeof(int))              // 4
print(sizeof(arr))              // sizeof выражения
const int K = 7                 // настоящая C-константа
int m[2][2] = {{1, 2}, {3, 4}}  // вложенные инициализаторы
ptr/int q = xs
q += 2
print(*q, q - xs)               // арифметика указателей
```

### print с несколькими аргументами

```lu
print(1, 2.5, "three", True)    // → 1 2.5 three true
```

### Единый вывод типов

Ошибки вида «f-строка печатает float как строку» исчезли как класс: тип
каждого выражения теперь вычисляется одним проходом по реестру классов
и символов (раньше — три независимые эвристики с разными слепыми зонами).

---

## Что нового в v3.3

### def-методы в классах и структурах

```lu
class Counter {
    int count
    def inc(int by) -> void {
        this.count = this.count + by
    }
    def get() -> int {
        return this.count
    }
}
```
Раньше `def` в теле класса ломал codegen — методы писались только через `Fn/`.
Теперь оба стиля работают в классах и в структурах, включая `virtual`/`override`.

### Vector<T> для любых типов элементов

Рантайм раньше был только для `int`. Теперь `Vector<str>`, `Vector<float>`, `Vector<bool>`, `Vector<byte>`, векторы пользовательских классов/структур — всё работает; код рантайма эмитится только для реально использованных типов.

```lu
Vector<str> names
names.push("alice")
print(names.get(0))   // alice
```

### Одинарные кавычки

```lu
str s = 'hello'
print(f"val={x > 5 ? 'big' : 'small'}")   // кавычки можно вкладывать
```

### Recv/ как выражение

```lu
Chan/ch
ch <- 42
print(Recv/ch)   // 42 — значение больше не выбрасывается
```

### Исправления (v3.3)

- `luc -v` показывал v3.0 — версия приведена к v3.3.
- f-строки: `{v.get(i)}` для `Vector<str>` больше не печатается как int.

---

## Что нового в v3.2

### Логические слова: and / or / not / True / False / None

```lu
bool ok = True
if a > 0 and not b {
    print("yes")
}
print(ok or False)   // true — булеаны печатаются как true/false, не 0/1
print(None)          // NULL
```

Оператор `in` — проверка подстроки:
```lu
if "ell" in "hello" {
    print("found")
}
```

### Inline-ассемблер (AT&T, GCC basic asm)

```lu
int gval = 40
asm {
    movq $2, %rax
    addq gval(%rip), %rax
    movq %rax, gval(%rip)
}
print(gval)   // 42
```
Текст сохраняется дословно; при наличии `asm`-блоков глобальные переменные эмитятся `volatile` (оптимизатор не видит записей из asm).

### Опциональная библиотека Tensor

```lu
#include <tensor.h>

Tensor a = tensor_zeros(2, 3)      // zeros / ones / full(r,c,v) / eye(n)
tensor_set(a, 0, 0, 1.5)           // set / fill
print(tensor_get(a, 0, 0))         // 1.5

Tensor c = tensor_add(a, b)        // elementwise: add / sub / mul / div
Tensor m = tensor_matmul(a, t2)    // [r,n] x [n,c] -> [r,c]
Tensor v = tensor_matvec(a, col)   // [r,c] x [c,1] -> [r,1]
print(tensor_dot(a, b))            // скалярное произведение равных форм

Tensor r = tensor_relu(x)          // relu / sigmoid / tanh / abs
Tensor s = tensor_scale(x, 2)      // умножение на скаляр
Tensor k = tensor_clip(x, -1, 1)   // ограничить диапазон
Tensor t = tensor_transpose(x)     // [r,c] -> [c,r]
Tensor d = tensor_copy(x)          // независимая копия данных

print(tensor_sum(x))               // sum / mean / max / min
print(tensor_argmax(x))            // индекс первого максимума
print(tensor_rows(x), tensor_cols(x))
tensor_print(m)                    // pretty-print Tensor[rows,cols]
tensor_free(m)                     // ровно один раз на каждый handle
```

Без точной директивы `#include <tensor.h>` тип `Tensor` и функции `tensor_*`
являются ошибкой компиляции. Реализация находится в `include/tensor.h`, а не в
сгенерированном C. Каждый конструктор и операция, возвращающая `Tensor`, создаёт
новый владеющий handle; освободите его ровно один раз через `tensor_free`.
Присваивание копирует только handle и не копирует данные — для независимой
копии есть `tensor_copy`.

Для сборки программы с тензорами укажите путь к библиотеке и `libm`:

```bash
gcc -O2 -std=c11 -Iinclude -o program output.c -lm
```

Неверные размеры, несовместимые формы, выход за границы, деление на ноль и
ошибки выделения памяти завершают программу с диагностикой в `stderr` и
кодом возврата 1. В диагностике печатаются обе формы — например
`tensor: matmul shape mismatch: [2,3] x [2,2]`.

Нативный бэкенд тензоры не поддерживает: `luc program.lu` без `-c` сразу
сообщает, какие две команды нужны. Для v5 всё то же самое, только библиотека
подключается через `incl`:

```lu
Lu/Strict

incl <tensor.h>

fn main() -> int {
    var a: Tensor = tensor_full(2, 2, 3)
    var e: Tensor = tensor_eye(2)
    var s: Tensor = tensor_add(a, e)
    tensor_print(s)

    var m: float = tensor_mean(s)   // float в v5 — это C double
    Pr(m)

    tensor_free(s)
    tensor_free(e)
    tensor_free(a)
    ret 0
}
```

В v5 нет float-литералов, поэтому скаляры передаются целыми (`tensor_full(2, 2, 3)`,
`tensor_clip(x, 0, 4)`); C приводит `int` к `double` при вызове. Значения `float`
печатаются через `Pr`, тензоры — через `tensor_print` (у `Uios` нет float-перегрузки).

### Исправления (v3.2)

- **`def main()` теперь работает**: не конфликтует с рантайм-точкой входа (переименовывается в `lu_user_main` и вызывается первой).
- **Экранирование скобок в f-строках**: `f"{{literal}}" → {literal}`.
- **Строковое `==`/`!=` сравнивает содержимое через `strcmp`**, а не указатели. Указательные сравнения (`fp == 0`, `p == null`) остались сравнением указателей.
- **Инлайн-булеаны печатаются как true/false** (`print(1 == 1)` → `true`), а не `0/1`.
- `bootstrap.sh` и `fuzz_test.sh` получили право выполнения.

---

## Что нового в v2.0

Lu v2.0 добавляет **компактный синтаксис** поверх существующего Lu-стиля, сохраняя **полную обратную совместимость**. Теперь можно писать код в гибридном стиле — старый и новый синтаксис работают вместе.

### Ключевые слова нового стиля

| Новый стиль | Lu-стиль (старый) | Описание |
|----------------------|-------------------|----------|
| `def f(x) -> int { }` | `Fn/f(x):int { }` | Объявление функции |
| `print(x)` / `print x` | `Pr/x` | Печать |
| `return x` | `Ret/x` | Возврат |
| `if cond { }` | `If/ cond { }` | Условие |
| `elif cond { }` | `Elif/ cond { }` | Иначе-если |
| `else { }` | `Else/ { }` | Иначе |
| `while cond { }` | `Loop/While cond { }` | Цикл while |
| `for x in expr { }` | `Loop/Each x in expr { }` | Цикл for-each |
| `break` | `Break/` | Выход из цикла |
| `continue` | — | Продолжить цикл |
| `auto x = expr` | — | Вывод типа |

### f-строки (интерполяция)

```lu
str name = "World"
int count = 42
print(f"Hello, {name}! Count: {count}")
print(f"Sum: {1 + 2 + 3}")
```

### Стандартная библиотека

**Math** (встроено через `<math.h>`):
```lu
print(sqrt(144))    // 12
print(max(3, 7))    // 7
print(min(3, 7))    // 3
print(abs(-42))     // 42
print(sin(3.14))    // ~0.0016
print(pow(2, 10))   // 1024
print(floor(3.7))   // 3
print(ceil(3.2))    // 4
```

**String**:
```lu
str s = "Hello World"
print(len(s))           // 11
print(upper(s))         // HELLO WORLD
print(lower(s))         // hello world
print(contains(s, "World"))  // true
print(replace(s, "World", "Lu"))  // Hello Lu
```

**IO**:
```lu
str content = read_file("input.txt")
write_file("output.txt", "data")
str name = input("Enter name: ")
```

**range()**:
```lu
for i in range(10) {
    print(i)
}
```

### Vector<T> — встроенный дженерик-контейнер

```lu
Vector<int> nums
nums.push(10)
nums.push(20)
nums.push(30)
print(nums.len())   // 3
print(nums.get(0))  // 10
print(nums.pop())   // 30

Vector<str> names       // тоже работает (v3.3): str/float/bool/byte,
names.push("alice")     // int64 и типы пользовательских классов/структур
```

### Умные указатели

```lu
// Unique<T> — авто-освобождение при выходе из scope
Unique<int> p = new int(42)
print(*p)  // 42
// p автоматически освобождается

// C-style указатели тоже работают
ptr/Vec2 ptr = new Vec2()
Free/ptr
```

### Конкатенация строк с авто-конверсией

```lu
str greeting = "Hello, " + "World" + "!"
str labeled = "Count: " + 42    // авто-конверсия int → str
```

### Составные операторы присваивания

```lu
x += 5    x -= 3    x *= 2    x /= 4    x %= 3
x &= 0xFF  x |= 0x100  x ^= 0xFF  x <<= 4  x >>= 2
```

---

## Быстрый старт

```bash
# Сборка компилятора
make

# Нативный бэкенд: свой статический ELF, gcc не нужен
./luc examples/v5/example.lu -o example
./example

# C-бэкенд: для GUI/X11, тензоров и всего, что требует C-библиотек
./luc -c examples/v5/example.lu -o example.c
gcc -O2 -std=c11 -o example example.c -lm
./example

# Полная регрессия (включая паритет двух бэкендов)
make test-all
make agent-test
```

---

## Документация

- [GUIDE.md](GUIDE.md) — полное руководство по языку, компилятору и подсистемам.
- [TUTORIAL.md](TUTORIAL.md) — пошаговый курс для начинающих.
- [PATCH_NOTES.md](PATCH_NOTES.md) — история изменений.

---

## Синтаксис за 30 секунд

### Новый стиль:

```lu
Lu/Language

def greet(str name) -> void {
    print(f"Привет, {name}!")
}

#q1
auto x = 42
for i in range(5) {
    print(i)
}
if x > 40 {
    print("big")
} else {
    print("small")
}
greet("мир")
#q1:end
```

### Lu-стиль (старый, по-прежнему работает):

```lu
Lu/Language

Fn/greet(str name):void {
    Pr/"Привет, "
    Pr/name
}

#q1
int x = 42
Loop/While x > 0 {
    Set/x = x - 1
}
If/ x == 0
To/ Pr/"Готово"
#q1:end
```

### Гибридный стиль (оба работают вместе):

```lu
Lu/Language

def factorial(int n) -> int {
    If/ n <= 1
    To/ Ret/1
    Ret/n * factorial(n - 1)
}

#q1
auto result = factorial(5)
print(f"5! = {result}")
Pr/result
#q1:end
```

---

## Опции компилятора

| Флаг | Описание |
|------|----------|
| `-n` | Нативный бэкенд: статический x86-64 ELF без libc (по умолчанию) |
| `-c` | C-бэкенд: сгенерировать C для gcc |
| `-o <file>` | Имя выходного файла (по умолчанию `<source>` для `-n`, `<source>.c` для `-c`) |
| `-O<0-3>` | Уровень оптимизации, записываемый в комментарий сгенерированного C (оптимизирует gcc — задавайте `-O` при сборке программы) |
| `-d` | Режим отладки |
| `-t` | Дамп токенов |
| `-a` | Дамп AST |
| `-s` | Статистика компиляции |
| `-v` | Версия |
| `-h` | Справка |

Если у `-o` суффикс `.c`, выбирается C-бэкенд — поэтому старая команда
`./luc foo.lu -o foo.c` работает как раньше.

**Важно**: при сборке сгенерированного C-кода добавляйте `-lm` для математической библиотеки:
```bash
gcc -O2 -std=c11 -o program output.c -lm
```

---

## Пример: GUI-калькулятор на Lu (X11)

`src/calculator_gui.lu` — калькулятор с окном, кнопками, мышью и клавиатурой,
написанный **целиком на Lu**: C-библиотека подключается через
`Import "<X11/Xlib.h>"`, вызовы (XOpenDisplay, XFillRectangle…) идут напрямую,
растровый шрифт 3×5 и вся логика — на Lu.

![Калькулятор на Lu](calculator_screenshot.png)

```lu
Lu/Language
Import "<X11/Xlib.h>"

// растровый шрифт 3×5 — 24 глифа
int FONT[24][5] = {
    {7, 5, 5, 5, 7},   // 0
    {2, 6, 2, 2, 7},   // 1
    ...
}

def apply_op(float a, float b, int op) -> float {
    if op == 43 { return a + b }
    if op == 47 {
        if b == 0 {
            err = 1
            return 0
        }
        return a / b
    }
    return b
}

def draw_glyph(int code, int px, int py, int s) -> void {
    for row in range(5) {
        int bits = FONT[code][row]
        for col in range(3) {
            if bits & (4 >> col) {
                XFillRectangle(g_d, g_win, g_gc, px + col * s, py + row * s, s, s)
            }
        }
    }
}
```

Сборка и запуск:

```bash
cd src
make test-gui                          # скомпилировать и слинковать
.lu_test_build/calculator_gui          # запустить (нужен X11-дисплей)
# или вручную:
./luc calculator_gui.lu -o calc.c
gcc -O2 -std=c11 -o calc calc.c -lX11
```

Возможности: 4×5 кнопок (мышь), клавиатура (цифры, `+ - * /`, Enter/=,
BackSpace, `c`/Escape — сброс, `n` — знак, `%` — процент), деление на ноль
→ `Error`. Окно закрывается крестиком (WM_DELETE_WINDOW).

---

## Шпаргалка: другой синтаксис → Lu

| Другой язык | Lu |
|-------------|----|
| `Vec2 v(1, 2);` | `Vec2 v = new Vec2(1, 2)` |
| `auto p = new Vec2(1, 2);` | `auto p = new Vec2(1, 2)` (→ `Vec2*`) |
| `auto c = a + b;` | `auto c = a + b` (op+ выводит класс) |
| `const int K = 7;` | `const int K = 7` |
| `sizeof(int)` | `sizeof(int)` |
| `int m[2][2] = {{1,2},{3,4}};` | `int m[2][2] = {{1,2},{3,4}}` |
| `std::vector<int> v; v.push_back(5);` | `Vector<int> v` / `v.push(5)` |
| `std::string s = a + std::to_string(42);` | `str s = "a" + 42` |
| `std::cout << "x=" << x << "\n";` | `print("x=", x)` |
| `struct S { void f() {...} };` | `struct S { Fn/f():void {...} }` |
| `asm("nop");` | `asm { nop }` (AT&T) |
| `x = cond ? a : b;` | `x = cond ? a : b` |
| `switch (x) { case 1: ... }` | `match x { case 1 { ... } case _ { ... } }` |
| `auto s = std::format("{}", x);` | `auto s = f"{x}"` |

---

## Ключевые возможности

- **Конструкторы с аргументами** (v4.0) — `new Vec2(1, 2)` для значений, указателей и `auto`
- **Ошибки arity** (v4.0) — неверное число аргументов = ошибка компиляции
- **sizeof / const / вложенные инициализаторы** (v4.0)
- **print с несколькими аргументами** (v4.0) — `print(a, b, c)`
- **Единый вывод типов** (v4.0) — поля, методы, перегрузки операторов, `auto`
- **Компактный синтаксис** — `def`, `print`, `if/elif/else`, `while`, `for-in`, `break`, `continue`, `and/or/not`, `True/False/None`, `in`
- **def-методы в классах и структурах** (v3.3) — наравне с `Fn/`
- **Vector<T> для любых типов элементов** (v3.3) — рантайм эмитится по использованным типам
- **Одинарные кавычки** (v3.3) — `'...'`, `f'...'`, вложение кавычек в f-строках
- **Recv/ как выражение** (v3.3) — `print(Recv/ch)`
- **auto / вывод типов** — компилятор сам определяет тип
- **f-строки** — `f"value: {x}"` интерполяция, экранирование `{{ }}`
- **Inline-asm** — `asm { ... }` (AT&T, basic asm)
- **Tensor-stdlib** — tensor_zeros/add/matmul/relu/print
- **Vector<T>** — встроенный дженерик-контейнер
- **Умные указатели** — `Unique<T>` с авто-освобождением
- **Стандартная библиотека** — math, string, io, range
- **Конкатенация строк** — `"a" + "b"`, `"count: " + 42`
- **Составные операторы** — `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- **OOP** — class, methods, this, new, ptr, Free
- **Управление памятью** — new/free, Unique<T>, Alloc/, Memset/, Memcpy/
- **Обработка ошибок** — Try/ / Catch/ / Finally/
- **Компиляция в C11** — нативная производительность
- **Самохостинг** — lu_compiler.lu написан на Lu
- **Полная обратная совместимость** — старый код работает без изменений

---

## Сеть, async, отладка (v1.x слой)

Lu имеет большой недокументированный слой из ранних версий — сетевые примитивы, асинхронность, события, отладка. Ниже — что **реально работает** (покрыто тестами) и что **сломано** (помечено явно).

### Работает (тестировано)

**Каналы:**
```lu
Chan/ch          // создать канал
ch <- 42         // отправить значение
Recv/ch          // получить (в codegen — lu_chan_recv)
print(Recv/ch)   // с v3.3 Recv/ работает и как выражение
```

**События:**
```lu
Event/click      // объявить событие
Emit/click "btn" // вызвать все обработчики
```

**Логирование:**
```lu
Log/info "starting"
Log/warn "low memory"
Log/err "failed"
Log/debug "trace info"
Log/fatal "crash"   // печатает и вызывает exit(1)
```

**Утверждения:**
```lu
Assert/x > 0 "x must be positive"
```

**Исключения:**
```lu
Try/ {
    Throw/ERR_MEM "out of memory"
}
Catch/ERR_MEM {
    Pr/"caught"
}
Finally/ {
    Pr/"cleanup"
}
```

Коды ошибок: `ERR_COR`, `ERR_IP`, `ERR_MEM`, `ERR_MSG`, `ERR_AUTH`.

### События с callback'ами (v3.1+)

```lu
def my_handler(void* data) -> void {
    Pr/"event fired!"
}

#q1
Event/click
On/click my_handler    // callback — имя функции, не строка!
Emit/click "button1"   // вызовет my_handler
#q1:end
```

**Важно:** `On/name callback` — `callback` должен быть именем функции (без кавычек). Строковые литералы отклоняются с понятной ошибкой.

### Асинхронность (v3.1+)

```lu
def my_task() -> void {
    Pr/"task running!"
}

#q1
Spawn/my_task    // имя функции, не число!
Pr/"after spawn"
#q1:end
```

### Удалённые сетевые заглушки

Эти v1.x конструкции не реализовывали сеть и были удалены. Компилятор отклоняет их с ошибкой:

- `Server #qN cor/{...}`
- `Snd{payload}`
- `Bcast{payload}`
- `Route/ dst via gateway`
- `Rec/`
- `inq;`

### Игровой движок (legacy)

Lu содержит "игровой движок" из ранних версий — `cr(obj;name)`, `dam(obj;name)`, `onl(...)`, `exp`, `cl`, `distance`. Эти токены определены в `lu.h`, парсятся и генерируют C-комментарии + вызовы к `LuObj_*` структурам, но **не документированы** как стабильная возможность. Не используйте в новом коде.

---

## Структура проекта

```
├── lu.h                  # Типы, AST, прототипы
├── main.c                # Точка входа CLI
├── lexer.c               # Лексер
├── parser.c              # Парсер (рекурсивный спуск)
├── semantic.c            # Семантический анализ (только для -c)
├── codegen.c             # Генератор C-кода + рантайм (-c)
├── v5.c                  # Транспайлер Lu v5 → ядро Classic
├── v5.h                  # Его интерфейс
├── x86.c                 # Нативный бэкенд: кодогенерация x86-64 + ELF (-n)
├── x86.h                 # Интерфейс нативного бэкенда
├── util.c                # Вспомогательные функции
├── lu_self_runtime.h     # Рантайм самохостинга
├── Makefile
├── bootstrap.sh          # Скрипт самохостинга
├── native_demo.lu        # Демонстрация нативного бэкенда
├── test_v20.lu           # Демонстрация v2.0 (новый синтаксис)
├── test_v13.lu           # Регрессии v1.3
├── test_paradigms.lu     # Демонстрация парадигм
├── lu_compiler.lu        # luc, написанный на Lu
├── calculator_gui.lu     # GUI-калькулятор на Lu + X11 (v4.0)
├── include/Uios.lu       # Библиотека потоков ввода-вывода: incl <Uios>
├── ../examples/          # Поставляемые программы
│   ├── classic/          #   Classic-оригиналы (baseline)
│   └── v5/               #   порты на Lu v5 (make test-ports)
└── ../tests/             # Регрессии: cli_test.sh, native_test.sh, v5_test.sh
    ├── native/*.lu       #   сборка обоими бэкендами, сверка stdout и rc
    ├── ports/            #   дифференциальный прогон портов Classic ↔ v5
    └── v5/good/*.lu      #   программы v5 + .stdout / .rc / .stdin
```

`tests/native/*.lu` — программы, которые собираются обоими бэкендами и должны
совпасть по stdout и коду возврата (`make test-native`).

`tests/v5/good/*.lu` — программы Lu v5: каждая собирается нативным
бэкендом, запускается с `.stdin` (если он есть) и сверяется с `.stdout` и
`.rc`, а при наличии `cc` — ещё и прогоняется через C-бэкенд, чтобы оба
бэкенда давали одинаковый результат (`make test-v5`).

`examples/v5/*.lu` — порты поставляемых программ. Каждый порт сверяется с
Classic-оригиналом из `examples/classic/` по stdout и коду возврата на
каждом бэкенде из `tests/ports/backends.txt` (`make test-ports`).

---

## Самохостинг

```bash
./bootstrap.sh
# ✓ BOOTSTRAP VERIFIED — output is identical
```

---

## Требования

- GCC ≥ 9 (C11) — только чтобы **собрать сам компилятор** и для бэкенда `-c`
- GNU Make
- Linux / macOS / WSL

Для `luc -n` на Linux x86-64 **gcc не нужен вообще**: компилятор пишет ELF
сам. Собранный `luc` — единственный инструмент, который требуется, чтобы
получить исполняемый файл.

---

## Лицензия

Lu Compiler v4.8 · 2026. Распространяется по [Apache License 2.0](LICENSE);
уведомления об авторстве — в [NOTICE](NOTICE).
