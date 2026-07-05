/*
 * Lu Doctor: small C regression runner for the Lu compiler.
 *
 * This file is intentionally plain C. It acts like a set of local
 * checking agents:
 *   syntax-agent    -> runs ./luc on generated Lu snippets
 *   c-backend-agent -> compiles generated C with gcc -std=c11
 *   runtime-agent   -> runs the native binary and checks stdout
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

typedef struct {
    const char *name;
    const char *source;
    const char *expected;
} LuCase;

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }
    if (fputs(text, f) < 0) {
        perror(path);
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int read_text(const char *path, char *buf, size_t cap) {
    FILE *f;
    size_t n;

    if (cap == 0) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 1;
}

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    return rc == 0;
}

static void make_workdir(void) {
#if defined(_WIN32)
    _mkdir(".lu_doctor");
#else
    mkdir(".lu_doctor", 0777);
#endif
}

static void print_log_tail(const char *path) {
    char buf[4096];
    if (read_text(path, buf, sizeof(buf))) {
        printf("--- %s ---\n%s\n", path, buf);
    }
}

static int run_case(const LuCase *tc) {
    char lu_path[256];
    char c_path[256];
    char exe_path[256];
    char out_path[256];
    char luc_log[256];
    char gcc_log[256];
    char cmd[1024];
    char out[8192];

    snprintf(lu_path, sizeof(lu_path), ".lu_doctor/%s.lu", tc->name);
    snprintf(c_path, sizeof(c_path), ".lu_doctor/%s.c", tc->name);
    snprintf(exe_path, sizeof(exe_path), ".lu_doctor/%s.out", tc->name);
    snprintf(out_path, sizeof(out_path), ".lu_doctor/%s.stdout", tc->name);
    snprintf(luc_log, sizeof(luc_log), ".lu_doctor/%s.luc.log", tc->name);
    snprintf(gcc_log, sizeof(gcc_log), ".lu_doctor/%s.gcc.log", tc->name);

    if (!write_text(lu_path, tc->source)) return 0;

    snprintf(cmd, sizeof(cmd), "./luc %s -o %s > %s 2>&1", lu_path, c_path, luc_log);
    if (!run_cmd(cmd)) {
        printf("[syntax-agent] FAIL %s\n", tc->name);
        print_log_tail(luc_log);
        return 0;
    }

    snprintf(cmd, sizeof(cmd),
             "gcc -std=c11 -Wall -Wextra -Wpedantic "
             "-Wno-unused-function -Wno-unused-variable -I. -o %s %s -lm > %s 2>&1",
             exe_path, c_path, gcc_log);
    if (!run_cmd(cmd)) {
        printf("[c-backend-agent] FAIL %s\n", tc->name);
        print_log_tail(gcc_log);
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "%s > %s 2>&1", exe_path, out_path);
    if (!run_cmd(cmd)) {
        printf("[runtime-agent] FAIL %s\n", tc->name);
        print_log_tail(out_path);
        return 0;
    }

    if (!read_text(out_path, out, sizeof(out))) {
        printf("[runtime-agent] FAIL %s: cannot read stdout\n", tc->name);
        return 0;
    }

    if (strcmp(out, tc->expected) != 0) {
        printf("[runtime-agent] FAIL %s: output mismatch\n", tc->name);
        printf("expected:\n%s", tc->expected);
        printf("actual:\n%s", out);
        return 0;
    }

    printf("[ok] %s\n", tc->name);
    return 1;
}

int main(void) {
    LuCase cases[] = {
        {
            "division_power_bool",
            "Lu/Language\n"
            "#q1\n"
            "int x = 8 / 2\n"
            "Pr/x\n"
            "int y = 2 ** 3\n"
            "Pr/y\n"
            "bool ok = x == 4 && y == 8\n"
            "Pr/ok\n"
            "#q1:end\n",
            "4\n8\ntrue\n"
        },
        {
            "enum_aliases",
            "Lu/Language\n"
            "enum Color {\n"
            "  RED = 1,\n"
            "  BLUE = 2\n"
            "}\n"
            "#q1\n"
            "int x = RED\n"
            "Pr/x\n"
            "Color c = BLUE\n"
            "Pr/c\n"
            "#q1:end\n",
            "1\n2\n"
        },
        {
            "array_expr_size",
            "Lu/Language\n"
            "#q1\n"
            "int n = 3\n"
            "int arr[n] = {1,2,3}\n"
            "Pr/arr[2]\n"
            "#q1:end\n",
            "3\n"
        },
        {
            "try_catch_finally",
            "Lu/Language\n"
            "#q1\n"
            "Try/ {\n"
            "  Pr/\"before\"\n"
            "  Throw/ERR_MEM \"bad memory\"\n"
            "  Pr/\"after\"\n"
            "}\n"
            "Catch/ERR_MEM {\n"
            "  Pr/\"caught\"\n"
            "}\n"
            "Finally/ {\n"
            "  Pr/\"done\"\n"
            "}\n"
            "Pr/\"next\"\n"
            "#q1:end\n",
            "before\ncaught\ndone\nnext\n"
        },
        {
            "class_method",
            "Lu/Language\n"
            "class Point {\n"
            "  pub int x\n"
            "  pub int y\n"
            "  pub Fn/sum():int {\n"
            "    Ret/this.x + this.y\n"
            "  }\n"
            "}\n"
            "#q1\n"
            "Point p\n"
            "Set/p.x = 2\n"
            "Set/p.y = 3\n"
            "Pr/p.x + p.y\n"
            "Pr/p.sum()\n"
            "#q1:end\n",
            "5\n5\n"
        },
        {
            "new_delete",
            "Lu/Language\n"
            "class Box {\n"
            "  pub int value\n"
            "}\n"
            "#q1\n"
            "ptr/Box b = new Box()\n"
            "Set/b->value = 99\n"
            "Pr/b->value\n"
            "delete b\n"
            "#q1:end\n",
            "99\n"
        },
        {
            "language_import",
            "Lu/Language\n"
            "Import Russian\n"
            "#q1\n"
            "Pr/\"ok\"\n"
            "#q1:end\n",
            "ok\n"
        },
        /* v1.3 regression cases */
        {
            "struct_array_field",
            "Lu/Language\n"
            "struct Buf {\n"
            "  int data[5]\n"
            "  int n\n"
            "}\n"
            "#q1\n"
            "Buf b\n"
            "b.data[0] = 10\n"
            "b.data[1] = 20\n"
            "b.data[2] = 30\n"
            "Pr/b.data[0] + b.data[1] + b.data[2]\n"
            "#q1:end\n",
            "60\n"
        },
        {
            "class_array_field_with_methods",
            "Lu/Language\n"
            "class Stack {\n"
            "  int data[100]\n"
            "  int top\n"
            "  Fn/init():void {\n"
            "    Set/this.top = 0\n"
            "  }\n"
            "  Fn/push(int v):void {\n"
            "    Set/this.data[this.top] = v\n"
            "    Set/this.top = this.top + 1\n"
            "  }\n"
            "  Fn/pop():int {\n"
            "    Set/this.top = this.top - 1\n"
            "    Ret/this.data[this.top]\n"
            "  }\n"
            "}\n"
            "#q1\n"
            "Stack s\n"
            "s.init()\n"
            "s.push(10)\n"
            "s.push(20)\n"
            "Pr/s.pop()\n"
            "Pr/s.pop()\n"
            "#q1:end\n",
            "20\n10\n"
        },
        {
            "compound_assignment",
            "Lu/Language\n"
            "#q1\n"
            "int x = 100\n"
            "x += 50\n"
            "x -= 25\n"
            "x *= 2\n"
            "Pr/x\n"
            "int arr[3] = {1, 2, 3}\n"
            "arr[1] += 100\n"
            "Pr/arr[1]\n"
            "#q1:end\n",
            "250\n102\n"
        },
        {
            "string_concatenation",
            "Lu/Language\n"
            "Fn/make_greeting(str name):str {\n"
            "  Ret/\"Hello, \" + name + \"!\"\n"
            "}\n"
            "#q1\n"
            "str s = make_greeting(\"World\")\n"
            "Pr/s\n"
            "str mixed = \"Count: \" + 42\n"
            "Pr/mixed\n"
            "#q1:end\n",
            "Hello, World!\nCount: 42\n"
        },
        {
            "loop_each_no_warning",
            "Lu/Language\n"
            "#q1\n"
            "int arr[5] = {1, 2, 3, 4, 5}\n"
            "int sum = 0\n"
            "Loop/Each item in arr {\n"
            "  sum += item\n"
            "}\n"
            "Pr/sum\n"
            "#q1:end\n",
            "15\n"
        },
        /* v2.0 Python-style syntax tests */
        {
            "python_style_keywords",
            "Lu/Language\n"
            "def add(int a, int b) -> int {\n"
            "  return a + b\n"
            "}\n"
            "#q1\n"
            "print(add(3, 4))\n"
            "auto x = 10\n"
            "print(x)\n"
            "while x > 7 {\n"
            "  print(x)\n"
            "  x -= 1\n"
            "}\n"
            "#q1:end\n",
            "7\n10\n10\n9\n8\n"
        },
        {
            "fstring_interpolation",
            "Lu/Language\n"
            "#q1\n"
            "int x = 42\n"
            "str name = \"Lu\"\n"
            "print(f\"{name} = {x}\")\n"
            "print(f\"sum = {1 + 2}\")\n"
            "#q1:end\n",
            "Lu = 42\nsum = 3\n"
        },
        {
            "range_and_for",
            "Lu/Language\n"
            "#q1\n"
            "int total = 0\n"
            "for i in range(5) {\n"
            "  total += i\n"
            "}\n"
            "print(total)\n"
            "for n in [10, 20, 30] {\n"
            "  print(n)\n"
            "}\n"
            "#q1:end\n",
            "10\n10\n20\n30\n"
        },
        {
            "vector_builtin",
            "Lu/Language\n"
            "#q1\n"
            "Vector<int> v\n"
            "v.push(100)\n"
            "v.push(200)\n"
            "v.push(300)\n"
            "print(v.len())\n"
            "print(v.get(0))\n"
            "print(v.get(2))\n"
            "#q1:end\n",
            "3\n100\n300\n"
        },
        {
            "math_functions",
            "Lu/Language\n"
            "#q1\n"
            "print(sqrt(25))\n"
            "print(max(3, 7))\n"
            "print(min(3, 7))\n"
            "#q1:end\n",
            "5\n7\n3\n"
        },
        {
            "string_functions",
            "Lu/Language\n"
            "#q1\n"
            "str s = \"Hello\"\n"
            "print(len(s))\n"
            "print(upper(s))\n"
            "print(contains(s, \"ell\"))\n"
            "#q1:end\n",
            "5\nHELLO\ntrue\n"
        },
        {
            "if_elif_else",
            "Lu/Language\n"
            "#q1\n"
            "int x = 5\n"
            "if x > 10 {\n"
            "  print(\"big\")\n"
            "} elif x > 3 {\n"
            "  print(\"medium\")\n"
            "} else {\n"
            "  print(\"small\")\n"
            "}\n"
            "#q1:end\n",
            "medium\n"
        },
        {
            "break_continue",
            "Lu/Language\n"
            "#q1\n"
            "for i in range(10) {\n"
            "  if i == 2 {\n"
            "    continue\n"
            "  }\n"
            "  if i == 5 {\n"
            "    break\n"
            "  }\n"
            "  print(i)\n"
            "}\n"
            "#q1:end\n",
            "0\n1\n3\n4\n"
        },
        {
            "unique_ptr",
            "Lu/Language\n"
            "#q1\n"
            "Unique<int> p = new int(99)\n"
            "print(*p)\n"
            "#q1:end\n",
            "99\n"
        },
        /* v2.1 f-string v2 and match/case tests */
        {
            "fstring_v2_complex",
            "Lu/Language\n"
            "def square(int n) -> int {\n"
            "  return n * n\n"
            "}\n"
            "class Point {\n"
            "  int x\n"
            "  int y\n"
            "  Fn/init(int ax, int ay):void {\n"
            "    Set/this.x = ax\n"
            "    Set/this.y = ay\n"
            "  }\n"
            "  Fn/mag():int {\n"
            "    Ret/this.x * this.x + this.y * this.y\n"
            "  }\n"
            "}\n"
            "#q1\n"
            "int x = 5\n"
            "str name = \"Lu\"\n"
            "print(f\"{name}: {x}, sq={square(x)}\")\n"
            "Point p\n"
            "p.init(3, 4)\n"
            "print(f\"mag={p.mag()}\")\n"
            "print(f\"{x > 3}\")\n"
            "#q1:end\n",
            "Lu: 5, sq=25\nmag=25\ntrue\n"
        },
        {
            "match_case",
            "Lu/Language\n"
            "#q1\n"
            "int x = 3\n"
            "match x {\n"
            "  case 1 {\n"
            "    print(\"one\")\n"
            "  }\n"
            "  case 2 {\n"
            "    print(\"two\")\n"
            "  }\n"
            "  case 3 {\n"
            "    print(\"three\")\n"
            "  }\n"
            "  case _ {\n"
            "    print(\"other\")\n"
            "  }\n"
            "}\n"
            "#q1:end\n",
            "three\n"
        },
        {
            "match_default",
            "Lu/Language\n"
            "#q1\n"
            "int code = 404\n"
            "match code {\n"
            "  case 200 {\n"
            "    print(\"OK\")\n"
            "  }\n"
            "  case 404 {\n"
            "    print(\"Not Found\")\n"
            "  }\n"
            "  case _ {\n"
            "    print(\"Unknown\")\n"
            "  }\n"
            "}\n"
            "#q1:end\n",
            "Not Found\n"
        },
        {
            "fstring_arithmetic",
            "Lu/Language\n"
            "#q1\n"
            "int a = 10\n"
            "int b = 20\n"
            "print(f\"{a} + {b} = {a + b}\")\n"
            "print(f\"{a * 2}, {b / 2}\")\n"
            "#q1:end\n",
            "10 + 20 = 30\n20, 10\n"
        },
        /* v2.1.1 segfault fix: array indexing in f-strings */
        {
            "fstring_array_index",
            "Lu/Language\n"
            "#q1\n"
            "int arr[5] = {10, 20, 30, 40, 50}\n"
            "int i = 1\n"
            "print(f\"{arr[0]}\")\n"
            "print(f\"{arr[i + 1]}\")\n"
            "print(f\"{arr[i * 2]}\")\n"
            "#q1:end\n",
            "10\n30\n30\n"
        },
        {
            "fstring_array_float",
            "Lu/Language\n"
            "#q1\n"
            "float farr[3] = {1.5, 2.5, 3.5}\n"
            "print(f\"{farr[1]}\")\n"
            "#q1:end\n",
            "2.5\n"
        }
    };
    int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0;
    int i;

    make_workdir();
    printf("=== Lu Doctor: C agent regression suite ===\n");
    for (i = 0; i < total; i++) {
        passed += run_case(&cases[i]);
    }
    printf("=== Result: %d/%d passed ===\n", passed, total);
    return passed == total ? 0 : 1;
}
