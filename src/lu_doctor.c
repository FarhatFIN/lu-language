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
             "-Wno-unused-function -Wno-unused-variable -I. -o %s %s > %s 2>&1",
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
