# luc — Lu Language Compiler v2.0

**Lu** — компилируемый язык программирования с **лёгким Python-подобным синтаксисом** и **низкоуровневыми возможностями C++**.  
Транслируется в C11. Компилятор написан на C и поддерживает **самохостинг**.

```
Lu/Language          →  luc  →  output.c  →  gcc  →  ./program
```

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
| `-O<0-3>` | Уровень оптимизации (по умолчанию 2) |
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

## Ключевые возможности

- **Python-подобный синтаксис** — `def`, `print`, `if/elif/else`, `while`, `for-in`, `break`, `continue`
- **auto / вывод типов** — компилятор сам определяет тип
- **f-строки** — `f"value: {x}"` интерполяция
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

Lu Compiler v2.0 · 2026
