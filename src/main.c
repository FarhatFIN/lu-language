#include "lu.h"
#include <getopt.h>
#include <time.h>

#define LUC_VERSION "1.1"

static void usage(const char *prog) {
    fprintf(stderr,
        "Lu Compiler  v" LUC_VERSION "\n"
        "Usage: %s [options] <source.lu>\n\n"
        "Options:\n"
        "  -o <file>    Output C file (default: <source>.c)\n"
        "  -O<0-3>      Optimisation level (default: 2)\n"
        "  -d           Debug mode (emit trace info into generated code)\n"
        "  -t           Dump tokens and exit\n"
        "  -a           Dump AST and exit\n"
        "  -s           Print compilation statistics\n"
        "  -v           Show version and exit\n"
        "  -h           Show this help\n\n"
        "After compilation, build with:\n"
        "  gcc -O<n> -std=c11 -o <program> <output.c>\n",
        prog);
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

int main(int argc, char **argv) {
    const char *outfile   = NULL;   /* NULL = auto-derive */
    int         opt_lvl   = 2;
    bool        debug     = false;
    bool        dump_tok  = false;
    bool        dump_ast  = false;
    bool        stats     = false;

    int c;
    while ((c = getopt(argc, argv, "o:O:dtasvh")) != -1) {
        switch (c) {
        case 'o': outfile  = optarg; break;
        case 'O':
            opt_lvl = atoi(optarg);
            if (opt_lvl < 0) opt_lvl = 0;
            if (opt_lvl > 3) opt_lvl = 3;
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
    const char *srcpath = argv[optind];

    /* ── Read source ── */
    char *src = read_file(srcpath);
    clock_t t0 = clock();
    fprintf(stderr, "[luc] compiling '%s'  (O%d%s)\n",
            srcpath, opt_lvl, debug ? " debug" : "");

    /* ── Lex ── */
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
        return 0;
    }

    /* ── Semantic analysis ── */
    SymTable syms = {0};
    semantic_check(root, &syms);
    if (stats)
        fprintf(stderr, "[luc] semantic check OK (%d symbols)\n", syms.count);

    /* ── Determine output file ── */
    char *auto_out = NULL;
    if (!outfile) {
        auto_out = derive_outpath(srcpath);
        outfile  = auto_out;
    }

    /* ── Code generation ── */
    FILE *out = fopen(outfile, "w");
    if (!out) { perror(outfile); return 1; }

    codegen_run(root, out, opt_lvl, debug);

    fclose(out);
    ast_free(root);
    free(src);

    clock_t t1 = clock();
    double ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;

    fprintf(stderr, "[luc] wrote '%s'  (%.1f ms)\n", outfile, ms);
    fprintf(stderr,
        "[luc] to build:\n"
        "      gcc -O%d -std=c11 -o program '%s'\n",
        opt_lvl, outfile);

    free(auto_out);
    return 0;
}
