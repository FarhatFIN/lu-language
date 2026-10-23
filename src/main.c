#include "lu.h"
#include "x86.h"
#include "v5.h"
#include <getopt.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define LUC_VERSION "4.5"

typedef enum { BACKEND_AUTO = -1, BACKEND_C = 0, BACKEND_NATIVE = 1 } Backend;

static void usage(const char *prog) {
    fprintf(stderr,
        "Lu Compiler  v" LUC_VERSION "\n"
        "Usage: %s [options] <source.lu>\n\n"
        "Options:\n"
        "  -n           Native backend: emit a static x86-64 ELF (default)\n"
        "  -c           C backend: emit C source for gcc\n"
        "  -o <file>    Output path (default: <source> for -n, <source>.c for -c)\n"
        "  -O<0-3>      Optimisation level (default: 2)\n"
        "  -d           Debug mode (emit trace info into generated code)\n"
        "  -t           Dump tokens and exit\n"
        "  -a           Dump AST and exit\n"
        "  -s           Print compilation statistics\n"
        "  -v           Show version and exit\n"
        "  -h           Show this help\n\n"
        "The native backend writes a freestanding Linux x86-64 executable that\n"
        "talks to the kernel directly — no gcc, no assembler, no linker.\n"
        "GUI, X11 and tensor builtins need the C backend (-c).\n"
        "Passing -o with a .c suffix selects the C backend.\n\n"
        "Run mode:\n"
        "  %s <source.lu> make\n"
        "      Compile with the native backend, run the program in this terminal\n"
        "      and exit with the program's own exit status. The binary is built\n"
        "      in $TMPDIR (or /tmp) and removed afterwards unless -o is given.\n",
        prog, prog);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) { perror(path); fclose(f); exit(1); }
    long sz = ftell(f);
    if (sz < 0) { perror(path); fclose(f); exit(1); }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fputs("out of memory\n", stderr); fclose(f); exit(1); }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    if (rd != (size_t)sz) {
        fprintf(stderr, "%s: short read (%zu of %ld bytes)\n", path, rd, sz);
        free(buf); fclose(f); exit(1);
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Derive output path from source: "foo/bar.lu" → "foo/bar.c" */
static char *derive_outpath(const char *src) {
    size_t len = strlen(src);
    char *out = malloc(len + 3); /* enough for replacing .lu with .c or appending .c */
    if (!out) { fputs("out of memory\n", stderr); exit(1); }
    strcpy(out, src);
    /* strip .lu extension if present */
    if (len > 3 && !strcmp(out + len - 3, ".lu"))
        out[len - 3] = '\0';
    strcat(out, ".c");
    return out;
}

/* ─────────────────────────────────────────────
   "make" mode helpers
   ───────────────────────────────────────────── */

/* Pick a fresh writable path for a throw-away binary. mkstemp() is POSIX-2008
   and stays hidden under -std=c11, so reserve the name with open(O_EXCL). */
static bool make_temp_path(char *buf, size_t cap) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    for (int i = 0; i < 4096; i++) {
        int n = snprintf(buf, cap, "%s/lu-make-%ld-%d", dir, (long)getpid(), i);
        if (n < 0 || (size_t)n >= cap) return false;
        int fd = open(buf, O_CREAT | O_EXCL | O_WRONLY, 0755);
        if (fd >= 0) { close(fd); return true; }
        if (errno != EEXIST) return false;
    }
    return false;
}

/* Run a freshly built binary and hand its exit status back to the caller.
   stdout/stderr/stdin are inherited, so the program prints straight into the
   terminal. A killed program reports 128+signal, like a shell does. */
static int run_binary(const char *path) {
    char local[4096];
    const char *run_path = path;
    if (!strchr(path, '/')) {                    /* execv() needs a path */
        snprintf(local, sizeof local, "./%s", path);
        run_path = local;
    }

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[luc] fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execv(run_path, (char *const[]){ (char *)run_path, NULL });
        fprintf(stderr, "[luc] cannot run '%s': %s\n", run_path, strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "[luc] waitpid failed: %s\n", strerror(errno));
            return 1;
        }
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "[luc] program killed by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *outfile   = NULL;   /* NULL = auto-derive */
    int         opt_lvl   = 2;
    bool        debug     = false;
    bool        dump_tok  = false;
    bool        dump_ast  = false;
    bool        stats     = false;
    Backend     backend   = BACKEND_AUTO;

    int c;
    while ((c = getopt(argc, argv, "nco:O:dtasvh")) != -1) {
        switch (c) {
        case 'n': backend  = BACKEND_NATIVE; break;
        case 'c': backend  = BACKEND_C; break;
        case 'o': outfile  = optarg; break;
        case 'O':
            if (strlen(optarg) != 1 || optarg[0] < '0' || optarg[0] > '3') {
                fprintf(stderr, "[luc] invalid optimisation level '%s': expected 0-3\n", optarg);
                return 1;
            }
            opt_lvl = optarg[0] - '0';
            break;
        case 'd': debug    = true; break;
        case 't': dump_tok = true; break;
        case 'a': dump_ast = true; break;
        case 's': stats    = true; break;
        case 'v':
            fprintf(stdout, "luc (Lu Compiler) v" LUC_VERSION "\n");
            return 0;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) { usage(argv[0]); return 1; }

    /* ./luc <source.lu> [make]
       The optional second positional must be the literal word "make".
       Anything else keeps the old one-source-file contract. */
    int  npos      = argc - optind;
    bool run_after = false;
    if (npos == 2 && strcmp(argv[optind + 1], "make") == 0) {
        run_after = true;
    } else if (npos != 1) {
        fprintf(stderr, "[luc] expected exactly one source file\n");
        fprintf(stderr, "[luc] usage: luc <source.lu> [make]\n");
        return 1;
    }
    const char *srcpath = argv[optind];

    if (run_after) {
        if (backend == BACKEND_C) {
            fprintf(stderr,
                "[luc] 'make' runs the native backend — it cannot be combined with -c\n");
            return 1;
        }
        backend = BACKEND_NATIVE;
        if (outfile) {
            size_t olen = strlen(outfile);
            if (olen > 2 && strcmp(outfile + olen - 2, ".c") == 0) {
                fprintf(stderr,
                    "[luc] 'make' writes an executable, not C source ('%s')\n", outfile);
                return 1;
            }
        }
    }

    /* ── Read source ── */
    char *src = read_file(srcpath);

    /* Lu/Strict (v5) sources are lowered to plain Lu text before anything
       else, so every backend accepts them without special cases. */
    if (v5_is_strict(src)) {
        char  err[512];
        char *v4 = v5_transpile(srcpath, src, err, sizeof err);
        if (!v4) {
            fprintf(stderr, "%s\n", err);
            free(src);
            return 1;
        }
        free(src);
        src = v4;
    }

    clock_t t0 = clock();
    fprintf(stderr, "[luc] compiling '%s'  (O%d%s)\n",
            srcpath, opt_lvl, debug ? " debug" : "");

    /* ── Lex ── */
    extern const char *g_lu_src;   /* сырой текст для asm-блоков (parser.c) */
    g_lu_src = src;
    Lexer *lexer = lexer_new(src);
    int   tok_count;
    Token *tokens = lexer_tokenize(lexer, &tok_count);
    lexer_free(lexer);
    if (stats)
        fprintf(stderr, "[luc] lexed %d tokens\n", tok_count);

    if (dump_tok) {
        for (int i = 0; i < tok_count; i++)
            printf("[%4d:%-3d] %-22s '%s'\n",
                   tokens[i].line, tokens[i].col,
                   token_type_name(tokens[i].type),
                   tokens[i].value ? tokens[i].value : "");
        tokens_free(tokens, tok_count);
        free(src);
        return 0;
    }

    /* ── Parse ── */
    ASTNode *root = parse(tokens, tok_count);
    tokens_free(tokens, tok_count);
    if (stats)
        fprintf(stderr, "[luc] parsed AST (%d top-level nodes)\n",
                root->children.count);

    if (dump_ast) {
        ast_print(root, 0);
        ast_free(root);
        free(src);
        return g_parse_error_count > 0 ? 1 : 0;
    }

    /* If there were parse errors, don't write invalid C — fail fast. */
    if (g_parse_error_count > 0) {
        fprintf(stderr, "[luc] %d parse error(s) — aborting (no output written)\n",
                g_parse_error_count);
        ast_free(root);
        free(src);
        return 1;
    }

    /* ── Determine output file and backend ── */
    char *auto_out = NULL;
    bool  temp_bin = false;   /* 'make' mode: throw-away binary in $TMPDIR */
    if (run_after && !outfile) {
        char tmpl[4096];
        if (!make_temp_path(tmpl, sizeof tmpl)) {
            fprintf(stderr, "[luc] cannot create a temporary file: %s\n",
                    strerror(errno));
            ast_free(root);
            free(src);
            return 1;
        }
        auto_out = malloc(strlen(tmpl) + 1);
        if (!auto_out) { fputs("out of memory\n", stderr); exit(1); }
        strcpy(auto_out, tmpl);
        outfile  = auto_out;
        temp_bin = true;
    } else if (!outfile) {
        if (backend == BACKEND_C)
            auto_out = derive_outpath(srcpath);           /* foo.lu → foo.c */
        else {
            size_t len = strlen(srcpath);
            auto_out = malloc(len + 1);
            if (!auto_out) { fputs("out of memory\n", stderr); exit(1); }
            strcpy(auto_out, srcpath);
            if (len > 3 && !strcmp(auto_out + len - 3, ".lu"))
                auto_out[len - 3] = '\0';
        }
        outfile = auto_out;
    }

    /* An explicit -n/-c wins; otherwise a .c output path keeps the old C
       behaviour so existing scripts and tests keep working unchanged. */
    if (backend == BACKEND_AUTO) {
        size_t olen = strlen(outfile);
        backend = (olen > 2 && !strcmp(outfile + olen - 2, ".c"))
                    ? BACKEND_C : BACKEND_NATIVE;
    }

    /* ── Semantic analysis (C backend only) ── */
    if (backend == BACKEND_C) {
        SymTable syms = {0};
        semantic_check(root, &syms);
        if (stats && g_parse_error_count == 0)
            fprintf(stderr, "[luc] semantic check OK (%d symbols)\n", syms.count);

        /* If semantic analysis found errors (undeclared identifiers etc.),
           the generated C won't compile — fail fast. */
        if (g_parse_error_count > 0) {
            fprintf(stderr, "[luc] %d error(s) during semantic analysis — aborting\n",
                    g_parse_error_count);
            ast_free(root);
            free(src);
            free(auto_out);
            return 1;
        }
    }

    /* tensor.h is a C header with doubles and libm; the native backend has
       neither and its generic "unknown type 'Tensor'" does not say how to
       build the file. */
    if (backend == BACKEND_NATIVE && has_tensor_import(root)) {
        fprintf(stderr,
                "[luc] tensor.h needs the C backend:\n"
                "      luc %s -c -o out.c\n"
                "      cc -std=c11 -Iinclude out.c -lm -o program\n", srcpath);
        ast_free(root);
        free(src);
        free(auto_out);
        return 1;
    }

    if (backend == BACKEND_NATIVE) {
        bool ok = native_compile(root, outfile, srcpath);
        ast_free(root);
        free(src);
        if (!ok) {
            fprintf(stderr, "[luc] native code generation failed\n");
            if (temp_bin) unlink(outfile);
            free(auto_out);
            return 1;
        }
        if (chmod(outfile, 0755) != 0)
            fprintf(stderr, "[luc] warning: could not mark '%s' executable\n", outfile);
        if (stats) {
            double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;
            fprintf(stderr, "[luc] native ELF written in %.1f ms\n", ms);
        }
        if (run_after) {
            /* Compile → run → show the program's output, then hand back its
               own exit status so scripts and `echo $?` stay honest. */
            int rc = run_binary(outfile);
            if (temp_bin) unlink(outfile);
            free(auto_out);
            return rc;
        }
        fprintf(stderr, "[luc] wrote '%s'  (static x86-64 ELF, no libc)\n", outfile);
        fprintf(stderr, "[luc] to run:\n      '%s'\n", outfile);
        free(auto_out);
        return 0;
    }

    /* ── C code generation ── */
    FILE *out = fopen(outfile, "w");
    if (!out) {
        perror(outfile);
        ast_free(root);
        free(src);
        free(auto_out);
        return 1;
    }

    codegen_run(root, out, opt_lvl, debug);

    /* Check both earlier writes and the final buffered write on close. */
    bool write_failed = ferror(out) != 0;
    if (fclose(out) != 0) write_failed = true;
    ast_free(root);
    free(src);

    if (write_failed) {
        fprintf(stderr, "[luc] failed to write output '%s'\n", outfile);
        free(auto_out);
        return 1;
    }

    /* If codegen found errors (e.g. On/ with string, Spawn/ with int),
       the output file may be invalid — return error. */
    if (g_parse_error_count > 0) {
        fprintf(stderr, "[luc] %d error(s) during code generation — output may be invalid\n",
                g_parse_error_count);
        free(auto_out);
        return 1;
    }

    double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;

    fprintf(stderr, "[luc] wrote '%s'  (%.1f ms)\n", outfile, ms);
    fprintf(stderr,
        "[luc] to build:\n"
        "      gcc -O%d -std=c11 -o program '%s'\n",
        opt_lvl, outfile);

    free(auto_out);
    return 0;
}
