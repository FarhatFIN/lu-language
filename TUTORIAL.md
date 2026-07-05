# Lu Language Tutorial

Пошаговое руководство: от Hello World до мессенджера.

---

## Установка

```bash
cd src
make
# Теперь у вас есть ./luc — компилятор Lu
```

---

## Урок 1: Hello World

Lu поддерживает два стиля синтаксиса одновременно — старый (Lu-стиль) и новый (Python-стиль). Начнём с Python-стиля.

Создайте `hello.lu`:

```lu
Lu/Language

#q1
print("Привет, мир!")
#q1:end
```

Компиляция и запуск:

```bash
./luc hello.lu -o hello.c
gcc -O2 -std=c11 -o hello hello.c -lm
./hello
# Вывод: Привет, мир!
```

**Что тут происходит?**
- `Lu/Language` — объявление языка (обязательно в первой строке)
- `#q1` ... `#q1:end` — исполняемый блок. Код внутри `_q1()` вызывается из `main()`
- `print(...)` — печать

Тот же код в старом Lu-стиле:

```lu
Lu/Language

#q1
Pr/"Привет, мир!"
#q1:end
```

Оба стиля работают вместе — выбирайте любой.

---

## Урок 2: Переменные и типы

```lu
Lu/Language

#q1
int x = 42
float pi = 3.14
str name = "Lu"
bool ok = true

print(x)
print(pi)
print(name)
print(ok)
#q1:end
```

**auto — вывод типа:**

```lu
auto a = 10        // → int
auto b = 2.5       // → float (double)
auto s = "hello"   // → str (char*)
auto flag = true   // → bool
```

---

## Урок 3: Функции

Python-стиль:

```lu
def add(int a, int b) -> int {
    return a + b
}

def greet(str name) -> void {
    print(f"Hello, {name}!")
}

#q1
print(add(3, 4))      // 7
greet("World")         // Hello, World!
#q1:end
```

Старый Lu-стиль:

```lu
Fn/add(int a, int b):int {
    Ret/a + b
}
```

---

## Урок 4: F-строки (v2)

F-строки поддерживают **любые выражения** в `{}`:

```lu
#q1
int x = 42
str name = "World"

print(f"Hello, {name}!")
print(f"x = {x}")
print(f"x squared = {x * x}")
print(f"{name} has {x} items")

// Вызовы функций в f-строках
def square(int n) -> int { return n * n }
print(f"square(5) = {square(5)}")

// Доступ к полям объектов
class Point {
    int x
    int y
    Fn/init(int ax, int ay):void {
        Set/this.x = ax
        Set/this.y = ay
    }
    Fn/mag():int {
        Ret/this.x * this.x + this.y * this.y
    }
}
Point p
p.init(3, 4)
print(f"Point({p.x}, {p.y}) mag={p.mag()}")

// Сравнения возвращают bool
print(f"x > 40: {x > 40}")
#q1:end
```

**Что поддерживается в `{}`:**
- Переменные: `{x}`
- Арифметика: `{x + y * 2}`
- Вызовы функций: `{square(5)}`
- Методы: `{p.mag()}`
- Доступ к полям: `{p.x}`
- Сравнения: `{x > 40}`
- Вложенные f-строки: `f"{f"x={x}"}"`

Типы выводятся автоматически: int → `lu_str_from_int`, float → `lu_str_from_float`, bool → `lu_str_from_bool`, str → как есть.

---

## Урок 5: Условия

```lu
#q1
int x = 10

if x > 20 {
    print("big")
} elif x > 5 {
    print("medium")
} else {
    print("small")
}
// Вывод: medium
#q1:end
```

Старый Lu-стиль:

```lu
If/ x > 20
To/ Pr/"big"
Elif/ x > 5
To/ Pr/"medium"
Else/
Pr/"small"
```

---

## Урок 6: Циклы

### for с range()

```lu
#q1
for i in range(5) {
    print(i)
}
// 0, 1, 2, 3, 4
#q1:end
```

### for со списком

```lu
#q1
for name in ["Алиса", "Борис", "Карл"] {
    print(name)
}
#q1:end
```

### while

```lu
#q1
int i = 0
while i < 5 {
    print(i)
    i += 1
}
#q1:end
```

### break и continue

```lu
#q1
for i in range(10) {
    if i == 3 { continue }
    if i == 7 { break }
    print(i)
}
// 0, 1, 2, 4, 5, 6
#q1:end
```

---

## Урок 7: match/case

Pattern matching в стиле Python 3.10:

```lu
#q1
int code = 404

match code {
    case 200 {
        print("OK")
    }
    case 404 {
        print("Not Found")
    }
    case 500 {
        print("Server Error")
    }
    case _ {
        print("Unknown")
    }
}
// Вывод: Not Found
#q1:end
```

`case _` — это default (срабатывает, если ничего не подошло).

---

## Урок 8: Классы и ООП

```lu
class Stack {
    int data[100]
    int top

    Fn/init():void {
        Set/this.top = 0
    }

    Fn/push(int v):void {
        Set/this.data[this.top] = v
        Set/this.top = this.top + 1
    }

    Fn/pop():int {
        Set/this.top = this.top - 1
        Ret/this.data[this.top]
    }

    Fn/size():int {
        Ret/this.top
    }
}

#q1
Stack s
s.init()
s.push(10)
s.push(20)
s.push(30)
print(s.size())    // 3
print(s.pop())     // 30
print(s.size())    // 2
#q1:end
```

**Указатели на объекты:**

```lu
#q1
ptr/Stack p = new Stack()
p.init()
p.push(100)
print(p.size())    // 1
Free/p
#q1:end
```

---

## Урок 9: Стандартная библиотека

### Math

```lu
#q1
print(sqrt(144))     // 12
print(pow(2, 10))    // 1024
print(max(3, 7))     // 7
print(min(3, 7))     // 3
print(abs(-42))      // 42
print(floor(3.7))    // 3
print(ceil(3.2))     // 4
print(sin(3.14))     // ~0.0016
#q1:end
```

### String

```lu
#q1
str s = "Hello World"
print(len(s))                    // 11
print(upper(s))                  // HELLO WORLD
print(lower(s))                  // hello world
print(contains(s, "World"))      // true
print(replace(s, "World", "Lu")) // Hello Lu
#q1:end
```

### IO

```lu
#q1
// Чтение файла
str content = read_file("input.txt")
print(content)

// Запись файла
write_file("output.txt", "Hello!")

// Ввод от пользователя
str name = input("Введите имя: ")
print(f"Привет, {name}!")
#q1:end
```

---

## Урок 10: Vector<T>

Встроенный дженерик-контейнер:

```lu
#q1
Vector<int> nums
nums.push(10)
nums.push(20)
nums.push(30)

print(nums.len())    // 3
print(nums.get(0))   // 10
print(nums.get(2))   // 30
print(nums.pop())    // 30
print(nums.len())    // 2
#q1:end
```

---

## Урок 11: Умные указатели

### Unique<T> — авто-освобождение

```lu
#q1
Unique<int> p = new int(42)
print(*p)    // 42
// p автоматически освобождается при выходе из scope
#q1:end
```

### Ручное управление

```lu
#q1
ptr/Stack s = new Stack()
s.init()
s.push(1)
Free/s    // явное освобождение
#q1:end
```

---

## Урок 12: Обработка ошибок

```lu
#q1
Try/ {
    Pr/"до ошибки"
    Throw/ERR_MEM "память закончилась"
    Pr/"после ошибки"    // не выполнится
}
Catch/ERR_MEM {
    Pr/"поймали ошибку памяти"
}
Finally/ {
    Pr/"очистка"
}
Pr/"продолжаем"
#q1:end
```

Вывод:
```
до ошибки
поймали ошибку памяти
очистка
продолжаем
```

---

## Урок 13: Структуры и перечисления

### struct

```lu
struct Point {
    int x
    int y
}

#q1
Point p
p.x = 10
p.y = 20
print(p.x + p.y)    // 30
#q1:end
```

### enum

```lu
enum Color {
    RED
    GREEN
    BLUE
}

enum Status {
    OK = 200
    NOT_FOUND = 404
    ERROR = 500
}

#q1
Color c = RED
print(c)            // 0
Status s = OK
print(s)            // 200
#q1:end
```

---

## Урок 14: Составные операторы

```lu
#q1
int x = 100
x += 50      // x = 150
x -= 25      // x = 125
x *= 2       // x = 250
x /= 5       // x = 50
x %= 3       // x = 2

int arr[5] = {1, 2, 3, 4, 5}
arr[2] += 100    // arr[2] = 103
#q1:end
```

Все операторы: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`

---

## Урок 15: Конкатенация строк

```lu
#q1
str a = "Hello, "
str b = "World!"
str c = a + b
print(c)    // Hello, World!

// С автоматической конверсией типов
str labeled = "Count: " + 42
print(labeled)    // Count: 42
#q1:end
```

---

## Урок 16: Каналы, события, логирование

Lu имеет слой из v1.x — асинхронные примитивы, события, отладка. Ниже — рабочий минимум.

### Каналы

```lu
#q1
Chan/ch           // создать канал
ch <- 42          // отправить
ch <- 100         // отправить ещё
Pr/"channel ok"
#q1:end
```

### События

```lu
#q1
Event/click       // объявить событие
Emit/click "btn"  // вызвать (но обработчиков пока нет)
Pr/"event ok"
#q1:end
```

**Важно:** `On/click callback` сейчас **сломан** — не используйте. Подробности в README, раздел "Не работает".

### Логирование

```lu
#q1
Log/info "starting"
Log/warn "low memory"
Log/err "failed"
Log/debug "trace"
#q1:end
```

Вывод (с цветами в терминале):
```
[Lu info] starting
[Lu warn] low memory
[Lu err] failed
[Lu debug] trace
```

`Log/fatal` печатает сообщение и вызывает `exit(1)`.

### Утверждения

```lu
#q1
int x = 5
Assert/x > 0 "x must be positive"
Pr/"ok"
#q1:end
```

Если условие ложно — печатает сообщение и завершает программу.

### Исключения

```lu
#q1
Try/ {
    Pr/"before"
    Throw/ERR_MEM "out of memory"
    Pr/"after"     // не выполнится
}
Catch/ERR_MEM {
    Pr/"caught memory error"
}
Finally/ {
    Pr/"cleanup"
}
Pr/"continue"
#q1:end
```

Коды ошибок: `ERR_COR`, `ERR_IP`, `ERR_MEM`, `ERR_MSG`, `ERR_AUTH`.

### Что НЕ работает

Эти конструкции компилируются, но генерируют невалидный C → segfault:

- `On/name callback` — callback передаётся как строка
- `Spawn/expr` — функция передаётся как int

Не используйте их. Это известные баги, задокументированные в README.

---

## Проект: Мессенджер

Теперь соберём всё вместе. Полный пример — в `src/messenger.lu`. Вот ключевые части:

```lu
Lu/Language

// Глобальные счётчики
int total_messages = 0

// Класс сообщения
class Message {
    str text
    str sender
    bool read

    Fn/init(str t, str s):void {
        Set/this.text = t
        Set/this.sender = s
        Set/this.read = false
    }

    Fn/format():str {
        str status = ""
        If/ this.read == false
        To/ Set/status = " *"
        Ret/"[" + this.sender + "] " + this.text + status
    }
}

// Функция отправки
def send_message(str from, str to, str text) -> void {
    print(f"[{from} -> {to}] {text}")
    total_messages += 1
}

#q1
Message m
m.init("Привет!", "Алиса")
print(m.format())

send_message("Алиса", "Борис", "Как дела?")

// Vector для статистики
Vector<int> daily
daily.push(12)
daily.push(25)
for day in range(daily.len()) {
    print(f"День {day}: {daily.get(day)} сообщений")
}

// match для обработки кодов
int code = 200
match code {
    case 200 { print("OK") }
    case 404 { print("Not Found") }
    case _ { print("Unknown") }
}

print(f"Всего: {total_messages}")
#q1:end
```

Компиляция и запуск:

```bash
./luc messenger.lu -o messenger.c
gcc -O2 -std=c11 -o messenger messenger.c -lm
./messenger
```

---

## Гибридный стиль

Lu v2.0 поддерживает **оба стиля одновременно**. Можно смешивать:

```lu
Lu/Language

// Новая функция (Python-стиль)
def factorial(int n) -> int {
    If/ n <= 1                    // старый Lu-стиль внутри
    To/ Ret/1
    Ret/n * factorial(n - 1)
}

#q1
auto result = factorial(5)        // auto — новый
print(f"5! = {result}")            // f-строка — новая
Pr/result                          // Pr/ — старый Lu-стиль
#q1:end
```

---

## Что дальше?

- **README.md** — полный список возможностей
- **PATCH_NOTES.md** — история изменений
- **src/messenger.lu** — полноценный пример мессенджера
- **src/test_v20.lu** — демонстрация всех v2.0 фич
- **src/test_v13.lu** — регрессионные тесты

### Команды для тестирования

```bash
make              # сборка компилятора
make test         # быстрый тест
make test-all     # все .lu файлы
make agent-test   # C-агенты (25/25 тестов)
./bootstrap.sh    # проверка самохостинга
```

---

## Сводка синтаксиса

| Конструкция | Python-стиль | Lu-стиль |
|-------------|--------------|----------|
| Функция | `def f(x) -> int { }` | `Fn/f(x):int { }` |
| Печать | `print(x)` | `Pr/x` |
| Возврат | `return x` | `Ret/x` |
| Условие | `if c { } elif c { } else { }` | `If/ c \n To/ ... \n Elif/ ... \n Else/` |
| Цикл while | `while c { }` | `Loop/While c { }` |
| Цикл for | `for x in range(n) { }` | `Loop/Each x in ... { }` |
| Присваивание | `x = expr` | `Set/x = expr` |
| match/case | `match x { case v { } case _ { } }` | — |
| f-строка | `f"{x}"` | — |
| auto | `auto x = expr` | — |

Удачного программирования на Lu! 🚀
