# luc — Lu Language Compiler v1.2

**Lu** — компилируемый язык программирования с синтаксисом, вдохновлённым естественным языком.  
Транслируется в C11. Компилятор написан на C и поддерживает **самохостинг**.

```
Lu/Language          →  luc  →  output.c  →  gcc  →  ./program
```

---

## Быстрый старт

```bash
# Сборка компилятора
make

# Компиляция примера
./luc example.lu -o example.c
gcc -O2 -std=c11 -o example example.c
./example

# Или одной командой
make test

# Full bundled regression pass
make test-all
```

---

## Синтаксис за 30 секунд

```lu
Lu/Language
M/ us *user*

Fn/greet(str name):void {
    Pr/"Привет, "
    Pr/name
}

#q1
int x = 42
Call/greet("мир")

Loop/While x > 0 {
    Set/x = x - 1
}

Pr/"Готово"
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

---

## Ключевые возможности

- **Компиляция в C11** — нативная производительность, легко интегрируется с существующим C-кодом
- **4 уровня языка** — от системных деклараций до сетевых примитивов
- **Управление памятью** — явные `Alloc/`, `Free/`, `Memset/`, `Memcpy/`
- **Структуры, перечисления, объединения** — полноценные составные типы
- **Async/Await + каналы** — встроенная асинхронность
- **Обработка ошибок** — `Try/` / `Catch/` / `Finally/`
- **OOP core lowering** — простые `class` → C `struct`, методы `Type_method(Type *this, ...)`, `this.field`, `v.method()`, `ptr/Type p = new Type()`
- **Самохостинг** — `lu_compiler.lu` — компилятор Lu, написанный на Lu

---

## Структура проекта

```
├── lu.h                  # Типы, AST, прототипы
├── main.c                # Точка входа CLI
├── lexer.c               # Лексер
├── parser.c              # Парсер (рекурсивный спуск)
├── semantic.c            # Семантический анализ
├── codegen.c             # Генератор C-кода
├── util.c                # Вспомогательные функции
├── lu_self_runtime.h     # Рантайм самохостинга
├── Makefile
├── bootstrap.sh          # Скрипт самохостинга
├── example.lu            # Учебный пример
├── lu_compiler.lu        # luc, написанный на Lu
├── kernel_sim.lu         # Симулятор микроядра
└── test_paradigms.lu     # Тесты
```


---

## Core+ пример

```lu
Lu/Language

#q1
Vec2 v
v.x = 6
v.y = 7
Pr/v.sum()

ptr/Vec2 p = new Vec2()
p.x = 20
p.y = 22
Pr/p.sum()
Free/p
#q1:end

class Vec2 {
    int x
    int y
    Fn/sum():int {
        Ret/this.x + this.y
    }
}
```

Сейчас это понижается в обычный C11: `Vec2` становится `struct`, метод становится функцией `Vec2_sum(Vec2 *this)`, а `v.sum()` становится `Vec2_sum(&v)`.

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

Lu Compiler v1.2 · 2026

---

## C doctor / local agents

This package includes a plain C regression runner:

```bash
cd src
make agent-test
```

`src/lu_doctor.c` creates small Lu programs, compiles them with `luc`, builds the generated C with GCC, runs the native binaries, and checks stdout. It covers arithmetic, power, enum aliases, array expression sizes, exceptions, class methods, `new/delete`, and language-only imports.
