# luc — Lu Language Compiler v4.0

**Lu** — компилируемый язык программирования с **лёгким Python-подобным синтаксисом**, **низкоуровневыми возможностями C++/asm** и **тензорной стандартной библиотекой в духе PyTorch**.  
Транслируется в C11. Компилятор написан на C и поддерживает **самохостинг**.

```
Lu/Language          →  luc  →  output.c  →  gcc  →  ./program
```

---

## Что нового в v4.0 — «Simple Core»

Уровень C++ и низкого уровня — без усложнения синтаксиса.

### Конструкторы с аргументами (C++-стиль)

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

### print как в Python

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

### Одинарные кавычки (Python-стиль)

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

### Python-стиль: and / or / not / True / False / None

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

### Tensor<T>-stdlib (PyTorch-flavor)

```lu
Tensor a = tensor_zeros(2, 3)      // zeros / ones / full(r,c,v) / eye(n)
tensor_set(a, 0, 0, 1.5)
print(tensor_get(a, 0, 0))         // 1.5
Tensor c = tensor_add(a, b)        // elementwise: add/sub/mul/div
Tensor m = tensor_matmul(a, t2)    // матричное умножение
Tensor r = tensor_relu(x)          // relu / sigmoid / scale(t, s) / transpose
print(tensor_sum(x))               // sum / mean / rows / cols
tensor_print(m)                    // pretty-print Tensor[rows,cols]
tensor_free(m)
```
Рантайм подключается к генерируемому C только если программа реально использует `tensor_*` — лишних зависимостей от libm у остальных программ нет.

### Исправления (v3.2)

- **`def main()` теперь работает**: не конфликтует с рантайм-точкой входа (переименовывается в `lu_user_main` и вызывается первой).
- **Экранирование скобок в f-строках**: `f"{{literal}}" → {literal}`.
- **Строковое `==`/`!=` сравнивает содержимое через `strcmp`**, а не указатели. Указательные сравнения (`fp == 0`, `p == null`) остались сравнением указателей.
- **Инлайн-булеаны печатаются как true/false** (`print(1 == 1)` → `true`), а не `0/1`.
- `bootstrap.sh` и `fuzz_test.sh` получили право выполнения.

---

## Что нового в v2.0

Lu v2.0 добавляет **Python-подобный синтаксис** поверх существующего Lu-стиля, сохраняя **полную обратную совместимость**. Теперь можно писать код в гибридном стиле — старый и новый синтаксис работают вместе.

### Python-стиль ключевые слова

| Python-стиль (новый) | Lu-стиль (старый) | Описание |
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

# Компиляция примера
./luc example.lu -o example.c
gcc -O2 -std=c11 -o example example.c -lm
./example

# Полная регрессия
make test-all
make agent-test
```

---

## Синтаксис за 30 секунд

### Python-стиль (новый):

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
| `-o <file>` | Имя выходного C-файла |
| `-O<0-3>` | Уровень, записываемый в комментарий сгенерированного C (оптимизирует gcc — задавайте `-O` при сборке программы) |
| `-d` | Режим отладки |
| `-t` | Дамп токенов |
| `-a` | Дамп AST |
| `-s` | Статистика компиляции |
| `-v` | Версия |

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

## Шпаргалка: C++ → Lu

| C++ | Lu |
|-----|----|
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
- **Python-подобный синтаксис** — `def`, `print`, `if/elif/else`, `while`, `for-in`, `break`, `continue`, `and/or/not`, `True/False/None`, `in`
- **def-методы в классах и структурах** (v3.3) — наравне с `Fn/`
- **Vector<T> для любых типов элементов** (v3.3) — рантайм эмитится по использованным типам
- **Одинарные кавычки** (v3.3) — `'...'`, `f'...'`, вложение кавычек в f-строках
- **Recv/ как выражение** (v3.3) — `print(Recv/ch)`
- **auto / вывод типов** — компилятор сам определяет тип
- **f-строки** — `f"value: {x}"` интерполяция, экранирование `{{ }}`
- **Inline-asm** — `asm { ... }` (AT&T, basic asm)
- **Tensor-stdlib** — tensor_zeros/add/matmul/relu/print в духе PyTorch
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

### Deprecated (будут удалены в v4.0)

Эти v1.x сетевые примитивы генерируют stub-код без реальной сетевой функциональности. Использование вызывает deprecation warning:

- `Server #qN cor/{...}` — stub `lu_server_new()`, без реального сервера
- `Snd{payload}` — stub `lu_send()`, без реальной отправки
- `Bcast{payload}` — stub `lu_broadcast()`, без реального broadcast
- `Route/ dst via gateway` — stub `lu_route()`, без реальной маршрутизации
- `Rec/` — stub `lu_recv()`, без реального приёма
- `inq;` — stub `lu_inq()`, без реального запроса
- `cor/{1,2,3}` — ключи корреляции (legacy)
- `Def:anw(expr)/Lin` — связанные ответы (legacy)

### Игровой движок (legacy)

Lu содержит "игровой движок" из ранних версий — `cr(obj;name)`, `dam(obj;name)`, `onl(...)`, `exp`, `cl`, `distance`. Эти токены определены в `lu.h`, парсятся и генерируют C-комментарии + вызовы к `LuObj_*` структурам, но **не документированы** как стабильная возможность. Не используйте в новом коде.

---

## Структура проекта

```
├── lu.h                  # Типы, AST, прототипы
├── main.c                # Точка входа CLI
├── lexer.c               # Лексер
├── parser.c              # Парсер (рекурсивный спуск)
├── semantic.c            # Семантический анализ
├── codegen.c             # Генератор C-кода + рантайм
├── util.c                # Вспомогательные функции
├── lu_self_runtime.h     # Рантайм самохостинга
├── Makefile
├── bootstrap.sh          # Скрипт самохостинга
├── example.lu            # Учебный пример (Lu-стиль)
├── test_v20.lu           # Демонстрация v2.0 (Python-стиль)
├── lu_compiler.lu        # luc, написанный на Lu
├── calculator_gui.lu     # GUI-калькулятор на Lu + X11 (v4.0)
└── test_*.lu             # Тесты
```

---

## Самохостинг

```bash
./bootstrap.sh
# ✓ BOOTSTRAP VERIFIED — output is identical
```

---

## Требования

- GCC ≥ 9 (C11)
- GNU Make
- Linux / macOS / WSL

---

## Лицензия

Lu Compiler v4.0 · 2026
