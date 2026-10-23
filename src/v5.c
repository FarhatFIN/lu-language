/* v5.c — front-end for Lu/Strict (syntax v5).
 *
 * The v5 surface is lowered to plain Lu (v4) source text and handed to the
 * existing lexer/parser/semantic/codegen pipeline.  Both the native x86-64
 * backend and the C backend therefore accept v5 programs unchanged.
 *
 * Lowering notes:
 *   vid x: T = e     ->  var x: T = e          (immutability is checked here)
 *   set x = e        ->  x = e
 *   pack X { ... }   ->  struct X { bits: int } + shift/mask helpers
 *   f.lit            ->  __lu_pkget(f.bits, shift, width)
 *   dv(&x)           ->  (syscall(0, 0, &x, size) > 0)
 *   Pr(a, b)         ->  print(a, b)
 *   Del p            ->  Free/p
 *   match            ->  if/else chain over a temporary
 *   a in arr         ->  hoisted loop into a temporary bool
 *   for i in a..b    ->  counted loop with an explicit counter
 */

/* readlink() and friends live behind the POSIX 2008 feature test macro. */
#define _POSIX_C_SOURCE 200809L

#include "v5.h"
#include "lu.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ─────────────────────────── tiny string buffer ─────────────────────────── */

typedef struct {
    char  *s;
    size_t len, cap;
} Buf;

static void buf_grow(Buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    {
        size_t cap = b->cap ? b->cap : 128;
        char  *p;
        while (cap < need) cap *= 2;
        p = realloc(b->s, cap);
        if (!p) { fputs("out of memory\n", stderr); exit(1); }
        b->s = p;
        b->cap = cap;
    }
}

static void buf_add(Buf *b, const char *s, size_t n)
{
    if (!n) return;
    buf_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_putc(Buf *b, char c) { buf_add(b, &c, 1); }

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char    tmp[512];
    va_list ap;
    int     n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { buf_add(b, tmp, (size_t)n); return; }
    {
        char *big = malloc((size_t)n + 1);
        if (!big) { fputs("out of memory\n", stderr); exit(1); }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        buf_add(b, big, (size_t)n);
        free(big);
    }
}

static void buf_reset(Buf *b)
{
    b->len = 0;
    if (b->s) b->s[0] = '\0';
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = malloc(n);
    if (!p) { fputs("out of memory\n", stderr); exit(1); }
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) { fputs("out of memory\n", stderr); exit(1); }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ──────────────────────────────── types ────────────────────────────────── */

enum {
    TY_BYTE = 1,
    TY_INT,
    TY_I64,
    TY_BOOL,
    TY_STR,
    TY_VOID,
    TY_NAMED,
    TY_PTR,
    TY_ARR
};

typedef struct Type Type;
struct Type {
    int         kind;
    const char *name;
    Type       *base;
    int         count;
};

static Type ty_byte = { TY_BYTE, "byte", NULL, 0 };
static Type ty_int  = { TY_INT,  "int",  NULL, 0 };
static Type ty_i64  = { TY_I64,  "int64", NULL, 0 };
static Type ty_bool = { TY_BOOL, "bool", NULL, 0 };
static Type ty_str  = { TY_STR,  "str",  NULL, 0 };
static Type ty_void = { TY_VOID, "void", NULL, 0 };

static Type *mk_type(int kind, const char *name, Type *base, int count)
{
    Type *t = malloc(sizeof *t);
    if (!t) { fputs("out of memory\n", stderr); exit(1); }
    t->kind  = kind;
    t->name  = name;
    t->base  = base;
    t->count = count;
    return t;
}

static Type *mk_ptr(Type *base) { return mk_type(TY_PTR, NULL, base, 0); }
static Type *mk_arr(Type *base, int n) { return mk_type(TY_ARR, NULL, base, n); }

static void type_text(Type *t, Buf *b)
{
    if (!t) { buf_puts(b, "int"); return; }
    switch (t->kind) {
    case TY_PTR: buf_puts(b, "ptr/"); type_text(t->base, b); break;
    case TY_ARR: type_text(t->base, b); buf_printf(b, "[%d]", t->count); break;
    default:     buf_puts(b, t->name ? t->name : "int"); break;
    }
}

static int type_size(Type *t)
{
    if (!t) return 4;
    switch (t->kind) {
    case TY_BYTE: case TY_BOOL: return 1;
    case TY_I64:  return 8;
    case TY_STR:  return 8;
    case TY_PTR:  return 8;
    case TY_ARR:  return type_size(t->base) * t->count;
    default:
        /* `float` is the C library's double: the tensor API returns it and
           the v4 layer maps it to double, so size it as 8 bytes. */
        return (t->name && !strcmp(t->name, "float")) ? 8 : 4;
    }
}

static int type_bits(Type *t)
{
    if (!t) return 32;
    if (t->kind == TY_BOOL) return 1;
    return type_size(t) * 8;
}

/* ──────────────────────────────── tokens ───────────────────────────────── */

enum { TK_EOF = 0, TK_ID, TK_NUM, TK_STR, TK_PUNCT };

typedef struct {
    int        kind;
    char      *text;
    long long  num;
    int        line, col;
    const char *file;          /* source file the token came from */
} Tok;

/* ─────────────────────────── symbols / program ─────────────────────────── */

typedef struct { char *name; Type *ty; bool is_vid; bool freed; } Var;

typedef struct { char *name; Type *ty; } Field;

typedef struct {
    char  *name;
    Type  *ret;
    char **pname;
    Type **ptype;
    int    np, cap;
    bool   self;
} Fn;

typedef struct { char *name; int shift, width; Type *ty; } Bit;

typedef struct { char *name; Bit *b; int nb, cap; } Pack;

typedef struct {
    char  *name;
    Field *f; int nf, cf;
    Fn    *m; int nm, cm;
} Struct;

typedef struct {
    const char *path;
    char       *dir;
    const char *cur_file;

    Tok *tk;
    int  nt, nt_alloc, pos;

    Buf out;
    int out_line;
    Buf pre;
    int depth;
    int blk_nloc[256];          /* nloc at each open block, restored on close */

    bool scan;
    bool failed;
    bool tensor_lib;            /* `incl <tensor.h>` was seen */
    char err[512];

    Var *glob; int nglob, cglob;
    Var *loc;  int nloc,  cloc;

    Fn     *fns; int nfn, cfn;
    Struct *sts; int nst, cst;
    Pack   *pks; int npk, cpk;

    int  tmp;
    int  last_bit_width;   /* width when the last expression was a pack field */
    bool last_was_strlit;
    bool last_was_literal;

    /* files pulled in with `incl <...>` (absolute paths, included once) */
    char **inc;
    int    ninc, cinc;

    /* loop contexts: 0 = normal, 1 = range-for with an explicit counter */
    int   loop_kind[64];
    char *loop_inc[64];
    int   nloop;
} V5;

/* ─────────────────────────────── utilities ─────────────────────────────── */

static void verr(V5 *v, int line, const char *fmt, ...)
{
    char        msg[400];
    const char *file = v->path;
    va_list     ap;
    if (v->failed) return;
    if (v->tk && v->pos < v->nt && v->tk[v->pos].file)
        file = v->tk[v->pos].file;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(v->err, sizeof v->err, "%s:%d: error: %s",
             file, line, msg);
    v->failed = true;
}

static Tok *cur(V5 *v) { return &v->tk[v->pos]; }

static Tok *peek(V5 *v, int off)
{
    int i = v->pos + off;
    if (i >= v->nt) i = v->nt - 1;
    return &v->tk[i];
}

static void adv(V5 *v) { if (v->tk[v->pos].kind != TK_EOF) v->pos++; }

static bool at_p(V5 *v, const char *p)
{
    return cur(v)->kind == TK_PUNCT && !strcmp(cur(v)->text, p);
}

static bool at_id(V5 *v, const char *s)
{
    return cur(v)->kind == TK_ID && !strcmp(cur(v)->text, s);
}

static bool at_kw(V5 *v, const char *s) { return at_id(v, s); }

static void expect_p(V5 *v, const char *p)
{
    if (v->failed) return;
    if (!at_p(v, p)) {
        verr(v, cur(v)->line, "expected '%s' before '%s'", p, cur(v)->text);
        return;
    }
    adv(v);
}

static Tok *expect_id(V5 *v, const char *what)
{
    if (v->failed) return cur(v);
    if (cur(v)->kind != TK_ID) {
        verr(v, cur(v)->line, "expected %s before '%s'", what, cur(v)->text);
        return cur(v);
    }
    return &v->tk[v->pos++];
}

static Var *find_local(V5 *v, const char *name)
{
    int i;
    for (i = 0; i < v->nloc; i++)
        if (!strcmp(v->loc[i].name, name)) return &v->loc[i];
    return NULL;
}

static Var *find_global(V5 *v, const char *name)
{
    int i;
    for (i = 0; i < v->nglob; i++)
        if (!strcmp(v->glob[i].name, name)) return &v->glob[i];
    return NULL;
}

static Var *find_var(V5 *v, const char *name)
{
    Var *x = find_local(v, name);
    return x ? x : find_global(v, name);
}

static void push_var(V5 *v, char *name, Type *ty, bool is_vid, bool global)
{
    if (find_var(v, name)) {
        verr(v, cur(v)->line, "redeclaration of '%s' (shadowing is forbidden)",
             name);
        return;
    }
    if (global) {
        if (v->nglob == v->cglob) {
            v->cglob = v->cglob ? v->cglob * 2 : 8;
            v->glob = realloc(v->glob, (size_t)v->cglob * sizeof *v->glob);
        }
        v->glob[v->nglob].name   = name;
        v->glob[v->nglob].ty     = ty;
        v->glob[v->nglob].is_vid = is_vid;
        v->glob[v->nglob].freed  = false;
        v->nglob++;
    } else {
        if (v->nloc == v->cloc) {
            v->cloc = v->cloc ? v->cloc * 2 : 8;
            v->loc = realloc(v->loc, (size_t)v->cloc * sizeof *v->loc);
        }
        v->loc[v->nloc].name   = name;
        v->loc[v->nloc].ty     = ty;
        v->loc[v->nloc].is_vid = is_vid;
        v->loc[v->nloc].freed  = false;
        v->nloc++;
    }
}

static Struct *find_struct(V5 *v, const char *name)
{
    int i;
    for (i = 0; i < v->nst; i++)
        if (!strcmp(v->sts[i].name, name)) return &v->sts[i];
    return NULL;
}

static Pack *find_pack(V5 *v, const char *name)
{
    int i;
    for (i = 0; i < v->npk; i++)
        if (!strcmp(v->pks[i].name, name)) return &v->pks[i];
    return NULL;
}

static Fn *find_fn(V5 *v, const char *name)
{
    int i;
    for (i = 0; i < v->nfn; i++)
        if (!strcmp(v->fns[i].name, name)) return &v->fns[i];
    return NULL;
}

static Struct *new_struct(V5 *v, char *name)
{
    Struct *s;
    if (v->nst == v->cst) {
        v->cst = v->cst ? v->cst * 2 : 8;
        v->sts = realloc(v->sts, (size_t)v->cst * sizeof *v->sts);
    }
    s = &v->sts[v->nst++];
    memset(s, 0, sizeof *s);
    s->name = name;
    return s;
}

static Fn *new_fn(V5 *v, char *name)
{
    Fn *f;
    if (find_fn(v, name)) {
        verr(v, cur(v)->line, "redeclaration of function '%s'", name);
        return find_fn(v, name);
    }
    if (v->nfn == v->cfn) {
        v->cfn = v->cfn ? v->cfn * 2 : 8;
        v->fns = realloc(v->fns, (size_t)v->cfn * sizeof *v->fns);
    }
    f = &v->fns[v->nfn++];
    memset(f, 0, sizeof *f);
    f->name = name;
    f->ret  = &ty_void;
    return f;
}

static void fn_param(Fn *f, char *name, Type *ty)
{
    if (f->np == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 4;
        f->pname = realloc(f->pname, (size_t)f->cap * sizeof *f->pname);
        f->ptype = realloc(f->ptype, (size_t)f->cap * sizeof *f->ptype);
    }
    f->pname[f->np] = name;
    f->ptype[f->np] = ty;
    f->np++;
}

static Field *struct_field(Struct *s, const char *name)
{
    int i;
    for (i = 0; i < s->nf; i++)
        if (!strcmp(s->f[i].name, name)) return &s->f[i];
    return NULL;
}

static Fn *struct_method(Struct *s, const char *name)
{
    int i;
    for (i = 0; i < s->nm; i++)
        if (!strcmp(s->m[i].name, name)) return &s->m[i];
    return NULL;
}

static Struct *struct_of(V5 *v, Type *t)
{
    if (!t) return NULL;
    if (t->kind == TY_PTR) t = t->base;
    if (t && t->kind == TY_NAMED) return find_struct(v, t->name);
    return NULL;
}

static Pack *pack_of(V5 *v, Type *t)
{
    if (!t) return NULL;
    if (t->kind == TY_PTR) t = t->base;
    if (t && t->kind == TY_NAMED) return find_pack(v, t->name);
    return NULL;
}

/* ───────────────────────────── lexer ───────────────────────────────────── */

static void add_tok(V5 *v, int kind, char *text, long long num, int line, int col)
{
    if (v->nt == v->nt_alloc) {
        v->nt_alloc = v->nt_alloc ? v->nt_alloc * 2 : 256;
        v->tk = realloc(v->tk, (size_t)v->nt_alloc * sizeof *v->tk);
    }
    v->tk[v->nt].kind = kind;
    v->tk[v->nt].text = text;
    v->tk[v->nt].num  = num;
    v->tk[v->nt].line = line;
    v->tk[v->nt].col  = col;
    v->tk[v->nt].file = v->cur_file;
    v->nt++;
}

static void lex_all(V5 *v, const char *src)
{
    const char *p = src;
    int line = 1, col = 1;

    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        p += 3;
        col += 3;
    }

    while (*p) {
        int tl, tc;
        if (*p == '\n') { line++; col = 1; p++; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; col++; continue; }

        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') { p++; col++; }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2; col += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') { line++; col = 1; p++; }
                else { p++; col++; }
            }
            if (*p) { p += 2; col += 2; }
            continue;
        }

        tl = line; tc = col;

        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '_') { p++; col++; }
            add_tok(v, TK_ID, xstrndup(s, (size_t)(p - s)), 0, tl, tc);
            continue;
        }

        if (isdigit((unsigned char)*p)) {
            long long val = 0;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2; col += 2;
                while (isxdigit((unsigned char)*p)) {
                    int d = isdigit((unsigned char)*p) ? *p - '0'
                            : (tolower((unsigned char)*p) - 'a' + 10);
                    val = val * 16 + d;
                    p++; col++;
                }
            } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
                p += 2; col += 2;
                while (*p == '0' || *p == '1' || *p == '_') {
                    if (*p != '_') val = val * 2 + (*p - '0');
                    p++; col++;
                }
            } else {
                while (isdigit((unsigned char)*p) || *p == '_') {
                    if (*p != '_') val = val * 10 + (*p - '0');
                    p++; col++;
                }
            }
            add_tok(v, TK_NUM, xstrdup(""), val, tl, tc);
            continue;
        }

        if (*p == '"' || ((unsigned char)p[0] == 0xC2 &&
                          (unsigned char)p[1] == 0xAB)) {
            Buf s;
            bool guillemet = (*p != '"');
            memset(&s, 0, sizeof s);
            if (guillemet) { p += 2; col += 2; }
            else           { p++;     col++;     }
            for (;;) {
                if (!*p) {
                    verr(v, tl, "unterminated string");
                    break;
                }
                if (guillemet) {
                    if ((unsigned char)p[0] == 0xC2 &&
                        (unsigned char)p[1] == 0xBB) {
                        p += 2; col += 2;
                        break;
                    }
                } else if (*p == '"') {
                    p++; col++;
                    break;
                }
                if (*p == '\\') {
                    p++; col++;
                    switch (*p) {
                    case 'n': buf_putc(&s, '\n'); break;
                    case 't': buf_putc(&s, '\t'); break;
                    case 'r': buf_putc(&s, '\r'); break;
                    case '0': buf_putc(&s, '\0'); break;
                    case '"': buf_putc(&s, '"');  break;
                    case '\\':buf_putc(&s, '\\'); break;
                    default:  buf_putc(&s, *p);   break;
                    }
                    p++; col++;
                    continue;
                }
                if (*p == '\n') { line++; col = 1; }
                else col++;
                buf_putc(&s, *p++);
            }
            add_tok(v, TK_STR, s.s ? s.s : xstrdup(""), 0, tl, tc);
            continue;
        }

        {
            static const char *two[] = {
                "->", "..", "==", "!=", "<=", ">=",
                "&&", "||", "<<", ">>", NULL
            };
            static const char *one = "+-*/%<>=!&|^()[]{},:.";
            int i;
            bool matched = false;
            for (i = 0; two[i]; i++) {
                if (p[0] == two[i][0] && p[1] == two[i][1]) {
                    add_tok(v, TK_PUNCT, xstrdup(two[i]), 0, tl, tc);
                    p += 2; col += 2;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            if (strchr(one, *p) && *p) {
                add_tok(v, TK_PUNCT, xstrndup(p, 1), 0, tl, tc);
                p++; col++;
                continue;
            }
            verr(v, tl, "unexpected character '%c'", *p);
            return;
        }
    }

    add_tok(v, TK_EOF, xstrdup(""), 0, line, col);
}

/* ─────────────────────────── output helpers ────────────────────────────── */

static void nl(V5 *v)
{
    buf_putc(&v->out, '\n');
    v->out_line++;
}

static void indent(V5 *v)
{
    int i;
    for (i = 0; i < v->depth; i++) buf_puts(&v->out, "    ");
}

static void align_to(V5 *v, int line)
{
    while (v->out_line < line) nl(v);
}

static void flush_pre(V5 *v)
{
    size_t i;
    int    lines = 0;
    if (!v->pre.len) return;
    buf_add(&v->out, v->pre.s, v->pre.len);
    for (i = 0; i < v->pre.len; i++)
        if (v->pre.s[i] == '\n') lines++;
    v->out_line += lines;
    buf_reset(&v->pre);
}

static void emit_line(V5 *v, int line, Buf *text)
{
    if (v->failed) return;
    align_to(v, line);
    flush_pre(v);
    indent(v);
    buf_add(&v->out, text->s ? text->s : "", text->len);
    nl(v);
    buf_reset(text);
}

static void open_block(V5 *v, Buf *head, int line)
{
    if (v->failed) return;
    align_to(v, line);
    flush_pre(v);
    indent(v);
    buf_add(&v->out, head->s ? head->s : "", head->len);
    buf_puts(&v->out, " {");
    nl(v);
    if (v->depth >= 0 && v->depth < 256) v->blk_nloc[v->depth] = v->nloc;
    v->depth++;
    buf_reset(head);
    expect_p(v, "{");
}

static void close_block(V5 *v)
{
    if (v->failed) return;
    if (v->depth > 0) v->depth--;
    if (v->depth >= 0 && v->depth < 256) v->nloc = v->blk_nloc[v->depth];
    indent(v);
    buf_puts(&v->out, "}");
    nl(v);
    expect_p(v, "}");
}

static void stmt(V5 *v);

static void body(V5 *v)
{
    while (!v->failed && !at_p(v, "}") && cur(v)->kind != TK_EOF)
        stmt(v);
}
/* ───────────────────────── type parsing ───────────────────────────────── */

static Type *parse_type(V5 *v);

static Type *parse_type_atom(V5 *v)
{
    Tok *t = expect_id(v, "a type name");
    if (v->failed) return &ty_int;
    if (!strcmp(t->text, "byte"))  return &ty_byte;
    if (!strcmp(t->text, "int"))   return &ty_int;
    if (!strcmp(t->text, "int64")) return &ty_i64;
    if (!strcmp(t->text, "bool"))  return &ty_bool;
    if (!strcmp(t->text, "str"))   return &ty_str;
    if (!strcmp(t->text, "void"))  return &ty_void;
    if (v->tensor_lib && !strcmp(t->text, "Tensor"))
        return mk_type(TY_NAMED, "Tensor", NULL, 0);
    /* `float` is the C double that tensor_get/sum and friends return. */
    if (v->tensor_lib && !strcmp(t->text, "float"))
        return mk_type(TY_NAMED, "float", NULL, 0);
    if (!strcmp(t->text, "Tensor")) {
        verr(v, t->line, "type 'Tensor' needs `incl <tensor.h>`");
        return &ty_int;
    }
    if (find_struct(v, t->text) || find_pack(v, t->text))
        return mk_type(TY_NAMED, t->text, NULL, 0);
    verr(v, t->line, "unknown type '%s'", t->text);
    return &ty_int;
}

static Type *parse_type(V5 *v)
{
    Type *t;
    if (v->failed) return &ty_int;
    if (at_id(v, "ptr") && peek(v, 1)->kind == TK_PUNCT &&
        !strcmp(peek(v, 1)->text, "/")) {
        adv(v);
        adv(v);
        return mk_ptr(parse_type_atom(v));
    }
    t = parse_type_atom(v);
    if (at_p(v, "[")) {
        int n;
        adv(v);
        if (cur(v)->kind != TK_NUM) {
            verr(v, cur(v)->line, "array length must be an integer literal");
            return t;
        }
        n = (int)cur(v)->num;
        adv(v);
        expect_p(v, "]");
        return mk_arr(t, n);
    }
    return t;
}

/* ───────────────────────── expression emission ─────────────────────────── */

static Type *expr(V5 *v, Buf *b);
static Type *unary(V5 *v, Buf *b);

static bool is_simple_ident(const char *s)
{
    if (!s || !*s) return false;
    if (!(isalpha((unsigned char)*s) || *s == '_')) return false;
    for (s++; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_')) return false;
    return true;
}

static void emit_str_lit(Buf *b, const char *s)
{
    buf_putc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { buf_putc(b, '\\'); buf_putc(b, (char)c); }
        else if (c == '\n') buf_puts(b, "\\n");
        else if (c == '\t') buf_puts(b, "\\t");
        else if (c == '\r') buf_puts(b, "\\r");
        else buf_putc(b, (char)c);
    }
    buf_putc(b, '"');
}

static bool is_num(Type *t)
{
    return t && (t->kind == TY_BYTE || t->kind == TY_INT || t->kind == TY_I64);
}

static bool compat(Type *d, Type *s)
{
    if (!d || !s) return true;
    if (d->kind == TY_VOID || s->kind == TY_VOID) return true;
    if (d->kind != s->kind) {
        if (s->kind == TY_INT &&
            (d->kind == TY_BYTE || d->kind == TY_I64)) return true;
        return false;
    }
    switch (d->kind) {
    case TY_NAMED:
        return d->name && s->name && !strcmp(d->name, s->name);
    case TY_PTR:
        return compat(d->base, s->base);
    case TY_ARR:
        return d->count == s->count && compat(d->base, s->base);
    default:
        return true;
    }
}

static const char *kind_name(int k)
{
    switch (k) {
    case TY_BYTE: return "byte";
    case TY_INT:  return "int";
    case TY_I64:  return "int64";
    case TY_BOOL: return "bool";
    case TY_STR:  return "str";
    case TY_VOID: return "void";
    case TY_PTR:  return "pointer";
    case TY_ARR:  return "array";
    default:      return "named type";
    }
}

static void require_bool(V5 *v, Type *t, int line)
{
    if (t && t->kind != TY_BOOL && t->kind != TY_VOID)
        verr(v, line, "condition must be bool, found %s", kind_name(t->kind));
}

/* Parse `( a, b, ... )` and append the argument list.  Returns false when
   the callee does not accept that many arguments. */
static void call_args(V5 *v, Buf *out, Fn *f, int line, int skip)
{
    Buf args;
    int n = 0;
    memset(&args, 0, sizeof args);
    expect_p(v, "(");
    while (!v->failed && !at_p(v, ")")) {
        Buf one;
        Type *at;
        memset(&one, 0, sizeof one);
        at = expr(v, &one);
        if (f && n + skip < f->np && !compat(f->ptype[n + skip], at))
            verr(v, line,
                 "argument %d of '%s' is %s, expected %s (use an explicit conversion)",
                 n + 1, f->name, kind_name(at->kind),
                 kind_name(f->ptype[n + skip]->kind));
        if (n) buf_puts(&args, ", ");
        buf_add(&args, one.s ? one.s : "", one.len);
        free(one.s);
        n++;
        if (at_p(v, ",")) { adv(v); continue; }
        break;
    }
    expect_p(v, ")");
    if (f && n != f->np - skip)
        verr(v, line, "'%s' expects %d argument(s), got %d",
             f->name, f->np - skip, n);
    buf_add(out, args.s ? args.s : "", args.len);
    free(args.s);
}

/* Read the `&x` inside dv(&x). */
static Type *dv_expr(V5 *v, Buf *b)
{
    Type *t = &ty_int;
    int line = cur(v)->line;
    adv(v); /* dv */
    expect_p(v, "(");
    if (!at_p(v, "&")) {
        verr(v, line, "dv expects a reference, e.g. dv(&x)");
        return &ty_bool;
    }
    adv(v);
    {
        Tok *n = expect_id(v, "a variable");
        Var *var = find_var(v, n->text);
        if (!var) {
            verr(v, n->line, "unknown variable '%s'", n->text);
            return &ty_bool;
        }
        if (var->ty->kind == TY_STR || var->ty->kind == TY_ARR) {
            verr(v, n->line, "dv cannot read into %s", kind_name(var->ty->kind));
            return &ty_bool;
        }
        t = var->ty;
    }
    expect_p(v, ")");
    buf_printf(b, "(syscall(0, 0, &%s, %d) > 0)", v->tk[v->pos - 2].text,
               type_size(t));
    return &ty_bool;
}

static Type *postfix(V5 *v, Buf *b, Type *t, size_t start);

static Type *primary(V5 *v, Buf *b)
{
    Tok *tk = cur(v);
    v->last_bit_width = 0;
    v->last_was_strlit = false;
    v->last_was_literal = false;

    if (v->failed) return &ty_int;

    if (tk->kind == TK_NUM) {
        adv(v);
        buf_printf(b, "%lld", (long long)tk->num);
        v->last_was_literal = true;
        return &ty_int;
    }

    if (tk->kind == TK_STR) {
        adv(v);
        emit_str_lit(b, tk->text);
        v->last_was_strlit = true;
        return &ty_str;
    }

    if (tk->kind == TK_PUNCT && !strcmp(tk->text, "(")) {
        Type *t;
        adv(v);
        /* No parentheses in the output: the v4 parser reads `(name)` as a
           C-style cast to a type called `name`.  Every operator already
           parenthesises itself, so dropping them is safe. */
        t = expr(v, b);
        expect_p(v, ")");
        return t;
    }

    if (tk->kind == TK_ID) {
        const char *s = tk->text;

        if (!strcmp(s, "true") || !strcmp(s, "false")) {
            adv(v);
            buf_puts(b, s);
            v->last_was_literal = true;
            return &ty_bool;
        }

        if (!strcmp(s, "dv") && peek(v, 1)->kind == TK_PUNCT &&
            !strcmp(peek(v, 1)->text, "("))
            return dv_expr(v, b);

        /* explicit conversions: int(x), byte(x), int64(x), bool(x) */
        if ((!strcmp(s, "int") || !strcmp(s, "byte") || !strcmp(s, "int64") ||
             !strcmp(s, "bool")) &&
            peek(v, 1)->kind == TK_PUNCT && !strcmp(peek(v, 1)->text, "(")) {
            Type *dst = !strcmp(s, "int") ? &ty_int
                      : !strcmp(s, "byte") ? &ty_byte
                      : !strcmp(s, "int64") ? &ty_i64 : &ty_bool;
            Buf inner;
            memset(&inner, 0, sizeof inner);
            adv(v);
            expect_p(v, "(");
            expr(v, &inner);
            expect_p(v, ")");
            buf_printf(b, "(%s)", s);
            if (is_simple_ident(inner.s)) {
                buf_add(b, inner.s, inner.len);
            } else {
                buf_putc(b, '(');
                buf_add(b, inner.s, inner.len);
                buf_putc(b, ')');
            }
            free(inner.s);
            return dst;
        }

        if (!strcmp(s, "Alloc"))
            verr(v, tk->line,
                 "Alloc(n) may only initialise a pointer declaration");

        /* syscall(nr, args...) — forwarded to the backend untouched */
        if (!strcmp(s, "syscall") && peek(v, 1)->kind == TK_PUNCT &&
            !strcmp(peek(v, 1)->text, "(")) {
            int n = 0;
            adv(v);
            buf_puts(b, "syscall(");
            expect_p(v, "(");
            while (!v->failed && !at_p(v, ")")) {
                Buf   arg;
                memset(&arg, 0, sizeof arg);
                expr(v, &arg);
                if (n) buf_puts(b, ", ");
                buf_add(b, arg.s ? arg.s : "", arg.len);
                free(arg.s);
                n++;
                if (at_p(v, ",")) { adv(v); continue; }
                break;
            }
            expect_p(v, ")");
            buf_putc(b, ')');
            return &ty_int;
        }

        /* tensor.h is a C library. v5 knows its signatures from the same
           table the v4 pipeline uses, so return types and arity are checked
           here and the calls reach the C backend unchanged. */
        if (!strncmp(s, "tensor_", 7) &&
            peek(v, 1)->kind == TK_PUNCT && !strcmp(peek(v, 1)->text, "(")) {
            const LibraryFunction *fn = v->tensor_lib ? tensor_function(s) : NULL;
            int n = 0;
            if (!v->tensor_lib) {
                verr(v, tk->line, "'%s' needs `incl <tensor.h>`", s);
                return &ty_int;
            }
            if (!fn) {
                verr(v, tk->line, "unknown tensor function '%s'", s);
                return &ty_int;
            }
            adv(v);
            buf_puts(b, s);
            buf_putc(b, '(');
            expect_p(v, "(");
            while (!v->failed && !at_p(v, ")")) {
                Buf arg;
                memset(&arg, 0, sizeof arg);
                expr(v, &arg);
                if (n) buf_puts(b, ", ");
                buf_add(b, arg.s ? arg.s : "", arg.len);
                free(arg.s);
                n++;
                if (at_p(v, ",")) { adv(v); continue; }
                break;
            }
            expect_p(v, ")");
            buf_putc(b, ')');
            if (n != fn->arity) {
                verr(v, tk->line, "'%s' expects %d argument(s), got %d",
                     fn->name, fn->arity, n);
                return &ty_int;
            }
            if (!strcmp(fn->result, "Tensor"))
                return mk_type(TY_NAMED, "Tensor", NULL, 0);
            if (!strcmp(fn->result, "float"))
                return mk_type(TY_NAMED, "float", NULL, 0);
            if (!strcmp(fn->result, "void")) return &ty_void;
            return &ty_int;
        }

        if (peek(v, 1)->kind == TK_PUNCT && !strcmp(peek(v, 1)->text, "(")) {
            Fn *f = find_fn(v, s);
            if (!f) {
                verr(v, tk->line, "unknown function '%s'", s);
                return &ty_int;
            }
            adv(v);
            buf_puts(b, s);
            buf_putc(b, '(');
            call_args(v, b, f, tk->line, 0);
            buf_putc(b, ')');
            return f->ret;
        }

        {
            Var *var = find_var(v, s);
            if (!var) {
                verr(v, tk->line, "undeclared name '%s'", s);
                return &ty_int;
            }
            if (var->freed)
                verr(v, tk->line, "use of '%s' after Del", s);
            adv(v);
            buf_puts(b, s);
            return var->ty;
        }
    }

    verr(v, tk->line, "unexpected token '%s'", tk->text);
    return &ty_int;
}

static Type *postfix(V5 *v, Buf *b, Type *t, size_t start)
{
    for (;;) {
        if (v->failed) return t;

        if (at_p(v, "[")) {
            Buf idx;
            Type *it;
            memset(&idx, 0, sizeof idx);
            adv(v);
            it = expr(v, &idx);
            expect_p(v, "]");
            (void)it;
            buf_putc(b, '[');
            buf_add(b, idx.s ? idx.s : "", idx.len);
            buf_putc(b, ']');
            free(idx.s);
            v->last_bit_width = 0;
            if (t->kind == TY_ARR)      t = t->base;
            else if (t->kind == TY_PTR) t = t->base;
            else                        t = &ty_int;
            continue;
        }

        if (at_p(v, ".")) {
            Tok  *ft;
            bool  arrow;
            Struct *st;
            Pack   *pk;
            adv(v);
            ft = expect_id(v, "a field or method name");
            if (v->failed) return t;
            arrow = (t->kind == TY_PTR);
            st = struct_of(v, t);
            pk = pack_of(v, t);

            /* method call? */
            if (peek(v, 0)->kind == TK_PUNCT && !strcmp(peek(v, 0)->text, "(")) {
                Fn *m;
                Buf base, call, args;
                if (!st) {
                    verr(v, ft->line, "type has no methods");
                    return t;
                }
                m = struct_method(st, ft->text);
                if (!m) {
                    verr(v, ft->line, "unknown method '%s' on '%s'",
                         ft->text, st->name);
                    return t;
                }
                memset(&base, 0, sizeof base);
                memset(&call, 0, sizeof call);
                memset(&args, 0, sizeof args);
                buf_add(&base, b->s + start, b->len - start);
                buf_printf(&call, "%s__%s(", st->name, ft->text);
                if (arrow) buf_add(&call, base.s, base.len);
                else {
                    buf_puts(&call, "(&");
                    buf_add(&call, base.s, base.len);
                    buf_puts(&call, ")");
                }
                call_args(v, &args, m, ft->line, 1);
                if (args.len) {
                    buf_puts(&call, ", ");
                    buf_add(&call, args.s, args.len);
                }
                buf_putc(&call, ')');
                free(base.s);
                free(args.s);
                b->len = start;
                buf_add(b, call.s, call.len);
                free(call.s);
                t = m->ret;
                v->last_bit_width = 0;
                continue;
            }

            if (pk) {
                int i;
                Bit *bit = NULL;
                Buf base, val;
                for (i = 0; i < pk->nb; i++)
                    if (!strcmp(pk->b[i].name, ft->text)) { bit = &pk->b[i]; break; }
                if (!bit) {
                    verr(v, ft->line, "unknown packed field '%s' in '%s'",
                         ft->text, pk->name);
                    return t;
                }
                memset(&base, 0, sizeof base);
                memset(&val, 0, sizeof val);
                buf_add(&base, b->s + start, b->len - start);
                buf_printf(&val, "__lu_pkget(%s%cbits, %d, %d)",
                           base.s, arrow ? '-' : '.', bit->shift, bit->width);
                b->len = start;
                if (bit->ty->kind == TY_BOOL) {
                    buf_putc(b, '(');
                    buf_add(b, val.s, val.len);
                    buf_puts(b, " != 0)");
                } else {
                    buf_putc(b, '(');
                    buf_add(b, val.s, val.len);
                    buf_putc(b, ')');
                }
                free(base.s);
                free(val.s);
                t = bit->ty;
                v->last_bit_width = bit->width;
                continue;
            }

            if (st) {
                Field *fl = struct_field(st, ft->text);
                if (!fl) {
                    verr(v, ft->line, "unknown field '%s' in '%s'",
                         ft->text, st->name);
                    return t;
                }
                buf_puts(b, arrow ? "->" : ".");
                buf_puts(b, ft->text);
                t = fl->ty;
                v->last_bit_width = 0;
                continue;
            }

            /* fall back to a plain field access */
            buf_puts(b, arrow ? "->" : ".");
            buf_puts(b, ft->text);
            t = &ty_int;
            v->last_bit_width = 0;
            continue;
        }

        return t;
    }
}

static Type *unary(V5 *v, Buf *b)
{
    size_t start;
    Type  *t;

    if (v->failed) return &ty_int;

    if (at_p(v, "-")) {
        adv(v);
        buf_puts(b, "(-");
        t = unary(v, b);
        buf_putc(b, ')');
        v->last_bit_width = 0;
        return t->kind == TY_I64 ? t : &ty_int;
    }
    if (at_p(v, "!")) {
        adv(v);
        buf_puts(b, "(!");
        unary(v, b);
        buf_putc(b, ')');
        v->last_bit_width = 0;
        return &ty_bool;
    }
    if (at_p(v, "&")) {
        adv(v);
        buf_puts(b, "(&");
        t = unary(v, b);
        buf_putc(b, ')');
        v->last_bit_width = 0;
        return mk_ptr(t);
    }

    start = b->len;
    t = primary(v, b);
    return postfix(v, b, t, start);
}

static int prec_of(V5 *v, Tok *t)
{
    if (t->kind == TK_ID && !strcmp(t->text, "in")) return 3;
    if (t->kind != TK_PUNCT) return 0;
    if (!strcmp(t->text, "||")) return 1;
    if (!strcmp(t->text, "&&")) return 2;
    if (!strcmp(t->text, "==") || !strcmp(t->text, "!=")) return 3;
    if (!strcmp(t->text, "<")  || !strcmp(t->text, "<=") ||
        !strcmp(t->text, ">")  || !strcmp(t->text, ">=")) return 4;
    if (!strcmp(t->text, "<<") || !strcmp(t->text, ">>")) return 5;
    if (!strcmp(t->text, "+")  || !strcmp(t->text, "-"))  return 6;
    if (!strcmp(t->text, "*")  || !strcmp(t->text, "/") ||
        !strcmp(t->text, "%"))  return 7;
    return 0;
}

/* Suffix used to pick the `put_<suffix>` / `get_<suffix>` method of a
   stream struct for a value of type `t`. */
static const char *stream_suffix(Type *t)
{
    switch (t->kind) {
    case TY_INT:   return "int";
    case TY_I64:   return "i64";
    case TY_BYTE:  return "byte";
    case TY_BOOL:  return "bool";
    case TY_STR:   return "str";
    case TY_PTR:   return "ptr";
    case TY_NAMED: return t->name;
    default:       return NULL;
    }
}

/* True when tokens [from, to) spell a place expression: name, name.f and
   name[i] chains.  `>>` needs one, because it hands out an address. */
static bool is_place_tokens(V5 *v, int from, int to)
{
    int i = from;
    if (i >= to || v->tk[i].kind != TK_ID) return false;
    i++;
    while (i < to) {
        if (v->tk[i].kind == TK_PUNCT && !strcmp(v->tk[i].text, ".")) {
            i++;
            if (i >= to || v->tk[i].kind != TK_ID) return false;
            i++;
        } else if (v->tk[i].kind == TK_PUNCT && !strcmp(v->tk[i].text, "[")) {
            int depth = 1;
            i++;
            while (i < to && depth > 0) {
                if (v->tk[i].kind == TK_PUNCT) {
                    if (!strcmp(v->tk[i].text, "[")) depth++;
                    else if (!strcmp(v->tk[i].text, "]")) depth--;
                }
                i++;
            }
            if (depth != 0) return false;
        } else {
            return false;
        }
    }
    return true;
}

static Type *binary(V5 *v, Buf *b, int minp);

/* `lhs in rhs` */
static Type *membership(V5 *v, Buf *b, size_t start, Type *lhs)
{
    Buf lhs_text;
    Type *elem = NULL;
    int   count = 0;
    bool  literal = false;

    memset(&lhs_text, 0, sizeof lhs_text);
    buf_add(&lhs_text, b->s + start, b->len - start);
    b->len = start;
    if (b->s) b->s[start] = '\0';

    if (at_p(v, "[")) {
        literal = true;
        adv(v);
        {
            int n = 0;
            buf_putc(b, '(');
            while (!v->failed && !at_p(v, "]")) {
                Buf one;
                Type *et;
                memset(&one, 0, sizeof one);
                et = expr(v, &one);
                if (!elem) elem = et;
                if (n) buf_puts(b, " || ");
                if (et->kind == TY_STR)
                    buf_printf(b, "__lu_streq(%s, %s)",
                               lhs_text.s, one.s ? one.s : "\"\"");
                else
                    buf_printf(b, "(%s == %s)",
                               lhs_text.s, one.s ? one.s : "0");
                free(one.s);
                n++;
                if (at_p(v, ",")) { adv(v); continue; }
                break;
            }
            if (!n) buf_puts(b, "false");
            buf_putc(b, ')');
        }
        expect_p(v, "]");
        (void)literal;
        free(lhs_text.s);
        return &ty_bool;
    }

    if (cur(v)->kind == TK_ID) {
        Tok *n = cur(v);
        Var *var = find_var(v, n->text);
        if (!var) {
            verr(v, n->line, "undeclared name '%s'", n->text);
            free(lhs_text.s);
            return &ty_bool;
        }
        if (var->ty->kind != TY_ARR) {
            verr(v, n->line, "right side of 'in' must be an array");
            free(lhs_text.s);
            return &ty_bool;
        }
        elem  = var->ty->base;
        count = var->ty->count;
        adv(v);
        {
            int id = v->tmp++;
            buf_printf(&v->pre, "var __lu_i%d: int = 0\n", id);
            buf_printf(&v->pre, "var __lu_f%d: bool = false\n", id);
            buf_printf(&v->pre, "loop %d {\n", count);
            if (elem->kind == TY_STR)
                buf_printf(&v->pre,
                           "    if __lu_streq(%s[__lu_i%d], %s) { __lu_f%d = true }\n",
                           n->text, id, lhs_text.s, id);
            else
                buf_printf(&v->pre,
                           "    if %s[__lu_i%d] == %s { __lu_f%d = true }\n",
                           n->text, id, lhs_text.s, id);
            buf_printf(&v->pre, "    __lu_i%d = __lu_i%d + 1\n", id, id);
            buf_puts(&v->pre, "}\n");
            buf_printf(b, "__lu_f%d", id);
        }
        free(lhs_text.s);
        return &ty_bool;
    }

    verr(v, cur(v)->line, "right side of 'in' must be an array");
    free(lhs_text.s);
    return &ty_bool;
}

static Type *binary(V5 *v, Buf *b, int minp)
{
    size_t start = b->len;
    Type  *lhs = unary(v, b);

    for (;;) {
        int  p = prec_of(v, cur(v));
        Buf  rhs;
        Type *rt, *res;
        const char *op;
        int  opline, rtok0, rtok1;

        if (v->failed || p < minp || p == 0) break;

        if (cur(v)->kind == TK_ID) {
            adv(v);
            lhs = membership(v, b, start, lhs);
            continue;
        }

        opline = cur(v)->line;
        op = cur(v)->text;
        adv(v);

        memset(&rhs, 0, sizeof rhs);
        rtok0 = v->pos;
        rt = binary(v, &rhs, p + 1);
        rtok1 = v->pos;

        /* `stream << value` / `stream >> place` call the stream's
           put_<kind> / get_<kind> method; anything numeric stays a shift. */
        if (!strcmp(op, "<<") || !strcmp(op, ">>")) {
            bool    write = !strcmp(op, "<<");
            Struct *st = struct_of(v, lhs);

            if (st) {
                const char *suf = stream_suffix(rt);
                char        mname[160];
                Fn         *m;
                Buf         call;

                if (!suf) {
                    verr(v, opline, "cannot stream a %s value", kind_name(rt->kind));
                    free(rhs.s);
                    return lhs;
                }
                if (!write && rt->kind != TY_INT && rt->kind != TY_I64 &&
                    rt->kind != TY_BYTE && rt->kind != TY_BOOL) {
                    verr(v, opline,
                         "'>>' reads into int, int64, byte or bool, not %s",
                         kind_name(rt->kind));
                    free(rhs.s);
                    return lhs;
                }
                snprintf(mname, sizeof mname, "%s_%s",
                         write ? "put" : "get", suf);
                m = struct_method(st, mname);
                if (!m) {
                    verr(v, opline, "'%s' has no method for %s with '%s'",
                         st->name, kind_name(rt->kind), op);
                    free(rhs.s);
                    return lhs;
                }
                if (m->np < 2 || !m->ptype[1]) {
                    verr(v, opline, "stream method '%s' takes no value", mname);
                    free(rhs.s);
                    return lhs;
                }
                if (write) {
                    if (!compat(m->ptype[1], rt)) {
                        verr(v, opline, "'%s' expects %s, found %s", mname,
                             kind_name(m->ptype[1]->kind), kind_name(rt->kind));
                        free(rhs.s);
                        return lhs;
                    }
                } else {
                    Type *want = m->ptype[1];
                    if (want->kind != TY_PTR || !compat(want->base, rt)) {
                        verr(v, opline, "'%s' expects a pointer to %s", mname,
                             kind_name(rt->kind));
                        free(rhs.s);
                        return lhs;
                    }
                    if (!is_place_tokens(v, rtok0, rtok1)) {
                        verr(v, opline,
                             "right side of '>>' must be a variable, field or element");
                        free(rhs.s);
                        return lhs;
                    }
                }

                memset(&call, 0, sizeof call);
                buf_printf(&call, "%s__%s(", st->name, mname);
                if (lhs->kind == TY_PTR) {
                    buf_add(&call, b->s + start, b->len - start);
                } else {
                    buf_puts(&call, "(&");
                    buf_add(&call, b->s + start, b->len - start);
                    buf_putc(&call, ')');
                }
                buf_puts(&call, ", ");
                if (!write) buf_putc(&call, '&');
                buf_add(&call, rhs.s ? rhs.s : "", rhs.len);
                buf_putc(&call, ')');

                b->len = start;
                if (b->s) b->s[start] = '\0';
                buf_add(b, call.s, call.len);
                free(call.s);
                free(rhs.s);
                lhs = m->ret ? m->ret : &ty_int;
                v->last_bit_width = 0;
                continue;
            }

            if (!is_num(lhs) || !is_num(rt)) {
                verr(v, opline, "operator '%s' needs numbers or a stream", op);
                free(rhs.s);
                return lhs;
            }
        }

        res = &ty_int;
        if (!strcmp(op, "||") || !strcmp(op, "&&") ||
            !strcmp(op, "==") || !strcmp(op, "!=") ||
            !strcmp(op, "<")  || !strcmp(op, "<=") ||
            !strcmp(op, ">")  || !strcmp(op, ">="))
            res = &ty_bool;
        if ((!strcmp(op, "<<") || !strcmp(op, ">>")) &&
            (lhs->kind == TY_I64 || rt->kind == TY_I64))
            res = &ty_i64;

        if ((!strcmp(op, "==") || !strcmp(op, "!=")) &&
            lhs->kind == TY_STR && rt->kind == TY_STR) {
            Buf tmp;
            memset(&tmp, 0, sizeof tmp);
            buf_printf(&tmp, "%s__lu_streq(%s, %s)",
                       !strcmp(op, "==") ? "(" : "(!",
                       b->s + start, rhs.s ? rhs.s : "\"\"");
            buf_puts(&tmp, ")");
            b->len = start;
            buf_add(b, tmp.s, tmp.len);
            free(tmp.s);
        } else {
            Buf tmp;
            memset(&tmp, 0, sizeof tmp);
            buf_putc(&tmp, '(');
            buf_add(&tmp, b->s + start, b->len - start);
            buf_printf(&tmp, " %s ", op);
            buf_add(&tmp, rhs.s ? rhs.s : "", rhs.len);
            buf_putc(&tmp, ')');
            b->len = start;
            buf_add(b, tmp.s, tmp.len);
            free(tmp.s);
        }
        free(rhs.s);
        lhs = res;
        v->last_bit_width = 0;
    }

    return lhs;
}

static Type *expr(V5 *v, Buf *b) { return binary(v, b, 1); }

/* ──────────────────────────── statements ───────────────────────────────── */

static void parse_bind(V5 *v);
static void parse_set(V5 *v);
static void parse_if(V5 *v);
static void parse_while(V5 *v);
static void parse_loop(V5 *v);
static void parse_for(V5 *v);
static void parse_match(V5 *v);
static void parse_ret(V5 *v);
static void parse_del(V5 *v);
static void parse_pr(V5 *v);
static void parse_prbits(V5 *v);
static void parse_memset(V5 *v);
static void parse_memcpy(V5 *v);

static void stmt(V5 *v)
{
    Tok *t;
    if (v->failed) return;
    t = cur(v);

    if (t->kind == TK_ID) {
        const char *s = t->text;
        if (!strcmp(s, "vid") || !strcmp(s, "var")) { parse_bind(v); return; }
        if (!strcmp(s, "set"))     { parse_set(v);    return; }
        if (!strcmp(s, "if"))      { parse_if(v);     return; }
        if (!strcmp(s, "while"))   { parse_while(v);  return; }
        if (!strcmp(s, "loop"))    { parse_loop(v);   return; }
        if (!strcmp(s, "for"))     { parse_for(v);    return; }
        if (!strcmp(s, "match"))   { parse_match(v);  return; }
        if (!strcmp(s, "ret"))     { parse_ret(v);    return; }
        if (!strcmp(s, "Del"))     { parse_del(v);    return; }
        if (!strcmp(s, "Pr"))      { parse_pr(v);     return; }
        if (!strcmp(s, "PrBits"))  { parse_prbits(v); return; }
        if (!strcmp(s, "Memset"))  { parse_memset(v); return; }
        if (!strcmp(s, "Memcpy"))  { parse_memcpy(v); return; }
        if (!strcmp(s, "break")) {
            int line = t->line;
            Buf b;
            memset(&b, 0, sizeof b);
            adv(v);
            buf_puts(&b, "break");
            emit_line(v, line, &b);
            free(b.s);
            return;
        }
        if (!strcmp(s, "continue")) {
            int line = t->line;
            Buf b;
            memset(&b, 0, sizeof b);
            adv(v);
            if (v->nloop > 0 && v->loop_kind[v->nloop - 1] == 1 &&
                v->loop_inc[v->nloop - 1]) {
                buf_printf(&b, "%s; continue", v->loop_inc[v->nloop - 1]);
            } else {
                buf_puts(&b, "continue");
            }
            emit_line(v, line, &b);
            free(b.s);
            return;
        }
    }

    if (t->kind == TK_ID && peek(v, 1)->kind == TK_PUNCT &&
        (!strcmp(peek(v, 1)->text, "(") ||
         !strcmp(peek(v, 1)->text, ".") ||
         !strcmp(peek(v, 1)->text, "<<") ||
         !strcmp(peek(v, 1)->text, ">>"))) {
        int line = t->line;
        Buf b;
        memset(&b, 0, sizeof b);
        expr(v, &b);
        emit_line(v, line, &b);
        free(b.s);
        return;
    }

    if (t->kind == TK_ID && peek(v, 1)->kind == TK_PUNCT &&
        !strcmp(peek(v, 1)->text, "=")) {
        if (find_var(v, t->text))
            verr(v, t->line, "assignment to '%s' requires 'set'", t->text);
        else
            verr(v, t->line, "undeclared name '%s'", t->text);
        return;
    }

    verr(v, t->line, "unexpected token '%s'", t->text);
}

/* array literal: [ e1, e2, ... ] */
static void parse_array_literal(V5 *v, int line, const char *name, Type *ty)
{
    int idx = 0;
    adv(v); /* [ */
    while (!v->failed && !at_p(v, "]")) {
        Buf one, out;
        Type *et;
        memset(&one, 0, sizeof one);
        memset(&out, 0, sizeof out);
        et = expr(v, &one);
        if (!compat(ty->base, et))
            verr(v, line, "array element %d is %s, expected %s",
                 idx, kind_name(et->kind), kind_name(ty->base->kind));
        if (idx >= ty->count)
            verr(v, line, "too many elements for '%s' (%d)", name, ty->count);
        buf_printf(&out, "%s[%d] = ", name, idx);
        buf_add(&out, one.s ? one.s : "", one.len);
        emit_line(v, line, &out);
        free(out.s);
        free(one.s);
        idx++;
        if (at_p(v, ",")) { adv(v); continue; }
        break;
    }
    expect_p(v, "]");
    if (!v->failed && idx != ty->count)
        verr(v, line, "'%s' expects %d element(s), got %d",
             name, ty->count, idx);
}

static void parse_bind(V5 *v)
{
    int   line = cur(v)->line;
    Tok  *kt = cur(v);
    bool  is_vid = !strcmp(kt->text, "vid");
    Tok  *nt;
    Type *ty;
    Buf   decl;
    bool  has_init = false, array_lit = false, alloc = false;

    adv(v);
    nt = expect_id(v, "a variable name");
    if (v->failed) return;

    if (!at_p(v, ":")) {
        verr(v, nt->line, "missing type for '%s'", nt->text);
        return;
    }
    adv(v);
    ty = parse_type(v);
    if (v->failed) return;

    memset(&decl, 0, sizeof decl);
    buf_printf(&decl, "var %s: ", nt->text);
    type_text(ty, &decl);

    if (at_p(v, "=")) {
        adv(v);
        if (at_id(v, "Alloc") && peek(v, 1)->kind == TK_PUNCT &&
            !strcmp(peek(v, 1)->text, "(")) {
            Buf cnt;
            if (ty->kind != TY_PTR) {
                verr(v, cur(v)->line, "Alloc needs a pointer type");
                return;
            }
            memset(&cnt, 0, sizeof cnt);
            adv(v);
            expect_p(v, "(");
            expr(v, &cnt);
            expect_p(v, ")");
            buf_puts(&decl, " = Alloc/");
            type_text(ty->base, &decl);
            buf_putc(&decl, ':');
            buf_add(&decl, cnt.s ? cnt.s : "0", cnt.len);
            free(cnt.s);
            has_init = true;
            alloc = true;
        } else if (at_p(v, "[")) {
            array_lit = true;
        } else {
            Buf init;
            Type *it;
            memset(&init, 0, sizeof init);
            it = expr(v, &init);
            if (!compat(ty, it)) {
                if (is_num(ty) && is_num(it))
                    verr(v, line,
                         "implicit conversion %s -> %s; write %s(...)",
                         kind_name(it->kind), kind_name(ty->kind),
                         kind_name(ty->kind));
                else
                    verr(v, line,
                         "cannot initialise '%s' (%s) with %s — use an explicit conversion",
                         nt->text, kind_name(ty->kind), kind_name(it->kind));
            }
            buf_puts(&decl, " = ");
            buf_add(&decl, init.s ? init.s : "", init.len);
            free(init.s);
            has_init = true;
        }
    }

    if (!array_lit && !has_init && ty->kind == TY_STR) {
        buf_puts(&decl, " = \"\"");
        has_init = true;
    }

    if (!array_lit) emit_line(v, line, &decl);
    free(decl.s);

    push_var(v, nt->text, ty, is_vid, false);
    (void)alloc;

    if (array_lit) {
        Buf head;
        memset(&head, 0, sizeof head);
        buf_printf(&head, "var %s: ", nt->text);
        type_text(ty, &head);
        emit_line(v, line, &head);
        free(head.s);
        parse_array_literal(v, line, nt->text, ty);
    }
}

typedef struct {
    Buf   text;
    char *pack_base;
    int   pack_shift, pack_width;
    int   subs;                 /* number of [..] / .field steps */
    Type *pack_type;
    Type *ty;
    Var  *var;
} Place;

static void parse_place(V5 *v, Place *pl)
{
    Tok *n;
    Type *t;
    memset(pl, 0, sizeof *pl);

    n = expect_id(v, "a variable");
    if (v->failed) return;
    pl->var = find_var(v, n->text);
    if (!pl->var) {
        verr(v, n->line, "undeclared name '%s'", n->text);
        return;
    }
    if (pl->var->freed)
        verr(v, n->line, "use of '%s' after Del", n->text);

    t = pl->var->ty;
    buf_puts(&pl->text, n->text);

    for (;;) {
        if (at_p(v, "[")) {
            Buf idx;
            memset(&idx, 0, sizeof idx);
            adv(v);
            expr(v, &idx);
            expect_p(v, "]");
            buf_putc(&pl->text, '[');
            buf_add(&pl->text, idx.s ? idx.s : "", idx.len);
            buf_putc(&pl->text, ']');
            pl->subs++;
            free(idx.s);
            if (t->kind == TY_ARR || t->kind == TY_PTR) t = t->base;
            else t = &ty_int;
            if (pl->pack_base) { free(pl->pack_base); pl->pack_base = NULL; }
            continue;
        }
        if (at_p(v, ".")) {
            Tok *ft;
            Struct *st;
            Pack *pk;
            bool arrow = (t->kind == TY_PTR);
            adv(v);
            ft = expect_id(v, "a field name");
            if (v->failed) break;
            pk = pack_of(v, t);
            if (pk) {
                int i;
                Bit *bit = NULL;
                for (i = 0; i < pk->nb; i++)
                    if (!strcmp(pk->b[i].name, ft->text)) { bit = &pk->b[i]; break; }
                if (!bit) {
                    verr(v, ft->line, "unknown packed field '%s' in '%s'",
                         ft->text, pk->name);
                    break;
                }
                free(pl->pack_base);
                pl->pack_base  = xstrdup(pl->text.s);
                pl->pack_shift = bit->shift;
                pl->pack_width = bit->width;
                pl->pack_type  = bit->ty;
                t = bit->ty;
                continue;
            }
            st = struct_of(v, t);
            if (st) {
                Field *fl = struct_field(st, ft->text);
                if (!fl) {
                    verr(v, ft->line, "unknown field '%s' in '%s'",
                         ft->text, st->name);
                    break;
                }
                t = fl->ty;
            } else {
                t = &ty_int;
            }
            buf_puts(&pl->text, arrow ? "->" : ".");
            buf_puts(&pl->text, ft->text);
            pl->subs++;
            if (pl->pack_base) { free(pl->pack_base); pl->pack_base = NULL; }
            continue;
        }
        break;
    }

    pl->ty = t;
}

static void parse_set(V5 *v)
{
    int   line = cur(v)->line;
    Place pl;

    adv(v); /* set */
    parse_place(v, &pl);
    if (v->failed) { free(pl.text.s); free(pl.pack_base); return; }

    if (pl.var && pl.var->is_vid &&
        !(pl.subs > 0 && pl.var->ty && pl.var->ty->kind == TY_PTR))
        verr(v, line, "cannot assign to immutable '%s'", pl.var->name);

    expect_p(v, "=");
    if (v->failed) { free(pl.text.s); free(pl.pack_base); return; }

    {
        Buf val;
        Type *vt;
        memset(&val, 0, sizeof val);
        vt = expr(v, &val);
        if (!compat(pl.ty, vt)) {
            if (is_num(pl.ty) && is_num(vt))
                verr(v, line, "implicit conversion %s -> %s; write %s(...)",
                     kind_name(vt->kind), kind_name(pl.ty->kind),
                     kind_name(pl.ty->kind));
            else
                verr(v, line,
                     "cannot assign %s to %s — use an explicit conversion",
                     kind_name(vt->kind), kind_name(pl.ty->kind));
        }

        if (pl.pack_base) {
            Buf out;
            memset(&out, 0, sizeof out);
            buf_printf(&out, "%s.bits = __lu_pkset(%s.bits, %d, %d, ",
                       pl.pack_base, pl.pack_base,
                       pl.pack_shift, pl.pack_width);
            if (pl.pack_type->kind == TY_BOOL) {
                buf_puts(&out, "__lu_b2i(");
                buf_add(&out, val.s ? val.s : "false", val.len);
                buf_putc(&out, ')');
            } else {
                buf_add(&out, val.s ? val.s : "0", val.len);
            }
            buf_puts(&out, ")");
            emit_line(v, line, &out);
            free(out.s);
        } else {
            Buf out;
            memset(&out, 0, sizeof out);
            buf_add(&out, pl.text.s, pl.text.len);
            buf_puts(&out, " = ");
            buf_add(&out, val.s ? val.s : "", val.len);
            emit_line(v, line, &out);
            free(out.s);
        }
        free(val.s);
    }
    free(pl.text.s);
    free(pl.pack_base);
}

static void parse_if(V5 *v)
{
    int line = cur(v)->line;
    Buf cond, head;
    Type *ct;

    memset(&cond, 0, sizeof cond);
    memset(&head, 0, sizeof head);
    adv(v);
    ct = expr(v, &cond);
    require_bool(v, ct, line);
    if (v->failed) { free(cond.s); return; }
    buf_puts(&head, "if ");
    buf_add(&head, cond.s, cond.len);
    free(cond.s);

    open_block(v, &head, line);
    body(v);

    while (!v->failed && at_p(v, "}") &&
           peek(v, 1)->kind == TK_ID && !strcmp(peek(v, 1)->text, "else")) {
        close_block(v);                    /* emits '}' and eats it */
        adv(v);                            /* else */
        if (at_id(v, "if")) {
            int l2 = cur(v)->line;
            Buf c2, h2;
            Type *t2;
            memset(&c2, 0, sizeof c2);
            memset(&h2, 0, sizeof h2);
            adv(v);
            t2 = expr(v, &c2);
            require_bool(v, t2, l2);
            buf_puts(&h2, "else if ");
            buf_add(&h2, c2.s ? c2.s : "false", c2.len);
            free(c2.s);
            open_block(v, &h2, l2);
            body(v);
            continue;
        }
        {
            Buf h3;
            memset(&h3, 0, sizeof h3);
            buf_puts(&h3, "else");
            open_block(v, &h3, cur(v)->line);
            body(v);
            break;
        }
    }
    close_block(v);
    free(head.s);
}

static void parse_while(V5 *v)
{
    int line = cur(v)->line;
    Buf cond, head;
    Type *ct;

    memset(&cond, 0, sizeof cond);
    memset(&head, 0, sizeof head);
    adv(v);
    ct = expr(v, &cond);
    require_bool(v, ct, line);
    if (v->failed) { free(cond.s); return; }
    buf_puts(&head, "while ");
    buf_add(&head, cond.s, cond.len);
    free(cond.s);

    v->loop_kind[v->nloop] = 0;
    v->loop_inc[v->nloop]  = NULL;
    v->nloop++;
    open_block(v, &head, line);
    body(v);
    close_block(v);
    v->nloop--;
    free(head.s);
}

static void parse_loop(V5 *v)
{
    int line = cur(v)->line;
    Buf cnt, head;
    Type *ct;

    memset(&cnt, 0, sizeof cnt);
    memset(&head, 0, sizeof head);
    adv(v);
    ct = expr(v, &cnt);
    (void)ct;
    if (v->failed) { free(cnt.s); return; }
    buf_puts(&head, "loop ");
    buf_add(&head, cnt.s, cnt.len);
    free(cnt.s);

    v->loop_kind[v->nloop] = 0;
    v->loop_inc[v->nloop]  = NULL;
    v->nloop++;
    open_block(v, &head, line);
    body(v);
    close_block(v);
    v->nloop--;
    free(head.s);
}

static void parse_for(V5 *v)
{
    int line = cur(v)->line;
    Tok *var;
    Buf first;
    Type *ft;

    adv(v); /* for */
    var = expect_id(v, "a loop variable");
    if (v->failed) return;
    if (!at_id(v, "in")) {
        verr(v, cur(v)->line, "expected 'in' in for loop");
        return;
    }
    adv(v);

    memset(&first, 0, sizeof first);
    ft = expr(v, &first);
    if (v->failed) { free(first.s); return; }

    if (at_p(v, "..")) {
        Buf end, out;
        int  id = v->tmp++;
        memset(&end, 0, sizeof end);
        memset(&out, 0, sizeof out);
        adv(v);
        expr(v, &end);

        buf_printf(&out, "var __lu_st%d: int = ", id);
        buf_add(&out, first.s ? first.s : "0", first.len);
        emit_line(v, line, &out);
        buf_reset(&out);

        buf_printf(&out, "var %s: int = __lu_st%d", var->text, id);
        emit_line(v, line, &out);
        buf_reset(&out);

        buf_printf(&out, "var __lu_en%d: int = ", id);
        buf_add(&out, end.s ? end.s : "0", end.len);
        emit_line(v, line, &out);
        buf_reset(&out);

        buf_printf(&out, "loop (__lu_en%d - __lu_st%d)", id, id);
        {
            int nloc_before = v->nloc;
            push_var(v, var->text, &ty_int, false, false);
            v->loop_kind[v->nloop] = 1;
            v->loop_inc[v->nloop]  = malloc(strlen(var->text) + 16);
            if (v->loop_inc[v->nloop])
                sprintf(v->loop_inc[v->nloop], "%s = %s + 1",
                        var->text, var->text);
            v->nloop++;
            open_block(v, &out, line);
            body(v);
            {
                Buf inc;
                memset(&inc, 0, sizeof inc);
                buf_printf(&inc, "%s = %s + 1", var->text, var->text);
                emit_line(v, v->out_line, &inc);
                free(inc.s);
            }
            close_block(v);
            v->nloop--;
            free(v->loop_inc[v->nloop]);
            v->loop_inc[v->nloop] = NULL;
            v->nloc = nloc_before;
        }
        free(first.s);
        free(end.s);
        return;
    }

    /* for x in array */
    if (ft->kind != TY_ARR) {
        verr(v, line, "for-in needs an array or a range");
        free(first.s);
        return;
    }
    {
        Buf head;
        memset(&head, 0, sizeof head);
        buf_printf(&head, "for %s in ", var->text);
        buf_add(&head, first.s ? first.s : "", first.len);
        {
            int nloc_before = v->nloc;
            push_var(v, var->text, ft->base, false, false);
            v->loop_kind[v->nloop] = 0;
            v->loop_inc[v->nloop]  = NULL;
            v->nloop++;
            open_block(v, &head, line);
            body(v);
            close_block(v);
            v->nloop--;
            v->nloc = nloc_before;
        }
        free(head.s);
        free(first.s);
    }
}

static void parse_match(V5 *v)
{
    int   line = cur(v)->line;
    int   id = v->tmp++;
    Buf   val;
    Type *vt;

    memset(&val, 0, sizeof val);
    adv(v); /* match */
    vt = expr(v, &val);
    if (v->failed) { free(val.s); return; }

    {
        Buf decl;
        memset(&decl, 0, sizeof decl);
        buf_printf(&decl, "var __lu_m%d: ", id);
        type_text(vt, &decl);
        buf_puts(&decl, " = ");
        buf_add(&decl, val.s ? val.s : "0", val.len);
        emit_line(v, line, &decl);
        free(decl.s);
    }
    free(val.s);

    expect_p(v, "{");

    {
        bool first = true;
        bool has_else = false;
        while (!v->failed && !at_p(v, "}") && cur(v)->kind != TK_EOF) {
            if (at_id(v, "else")) {
                int l2 = cur(v)->line;
                Buf head;
                adv(v);
                memset(&head, 0, sizeof head);
                buf_puts(&head, "else");
                open_block(v, &head, l2);
                body(v);
                close_block(v);
                has_else = true;
                break;
            }
            if (!at_id(v, "case")) {
                verr(v, cur(v)->line, "expected 'case' or 'else' in match");
                break;
            }
            adv(v);
            {
                int l2 = cur(v)->line;
                Buf pat, head;
                Type *pt;
                memset(&pat, 0, sizeof pat);
                memset(&head, 0, sizeof head);
                pt = expr(v, &pat);
                if (vt->kind == TY_STR && pt->kind == TY_STR)
                    buf_printf(&head, "%s __lu_streq(__lu_m%d, %s)",
                               first ? "if" : "else if", id,
                               pat.s ? pat.s : "\"\"");
                else
                    buf_printf(&head, "%s (__lu_m%d == %s)",
                               first ? "if" : "else if", id,
                               pat.s ? pat.s : "0");
                free(pat.s);
                first = false;
                open_block(v, &head, l2);
                body(v);
                close_block(v);
            }
        }
        if (!has_else && !v->failed && vt->kind != TY_BOOL)
            verr(v, line, "match on %s needs an else branch",
                 kind_name(vt->kind));
        if (!v->failed && at_p(v, "}")) adv(v);
    }
}

static void parse_ret(V5 *v)
{
    int line = cur(v)->line;
    Buf b;
    memset(&b, 0, sizeof b);
    adv(v);
    if (at_p(v, "}") || cur(v)->kind == TK_EOF) {
        buf_puts(&b, "return");
    } else {
        buf_puts(&b, "return ");
        expr(v, &b);
    }
    emit_line(v, line, &b);
    free(b.s);
}

static void parse_del(V5 *v)
{
    int line = cur(v)->line;
    Buf b;
    Tok *n;
    Var *var;

    memset(&b, 0, sizeof b);
    adv(v);
    n = expect_id(v, "a pointer");
    if (v->failed) return;
    var = find_var(v, n->text);
    if (!var) {
        verr(v, n->line, "undeclared name '%s'", n->text);
        return;
    }
    if (var->ty->kind != TY_PTR)
        verr(v, n->line, "'%s' is not a pointer", n->text);
    var->freed = true;
    buf_printf(&b, "Free/%s", n->text);
    emit_line(v, line, &b);
    free(b.s);
}

static void parse_pr(V5 *v)
{
    int line = cur(v)->line;
    Buf b;
    int  n = 0;
    bool first_is_str = false;

    memset(&b, 0, sizeof b);
    adv(v);
    buf_puts(&b, "print(");
    expect_p(v, "(");
    while (!v->failed && !at_p(v, ")")) {
        Buf one;
        memset(&one, 0, sizeof one);
        expr(v, &one);
        if (n == 0) first_is_str = v->last_was_strlit;
        if (n) {
            if (n == 1 && first_is_str && one.len &&
                strchr(one.s ? one.s : "", '%') &&
                (strstr(one.s, "%d") || strstr(one.s, "%s") ||
                 strstr(one.s, "%x") || strstr(one.s, "%u"))) {
                /* the second argument contains a format spec but the first
                   was a literal string: reject format strings */
            }
            buf_puts(&b, ", ");
        }
        buf_add(&b, one.s ? one.s : "", one.len);
        free(one.s);
        n++;
        if (at_p(v, ",")) { adv(v); continue; }
        break;
    }
    expect_p(v, ")");
    if (n == 0) buf_puts(&b, "\"\"");

    /* reject printf-style usage: a leading literal string with a % specifier
       and more than one argument */
    if (n > 1 && first_is_str) {
        /* the first argument text lives at the start of b after "print(" */
        const char *p = b.s ? strchr(b.s, '(') : NULL;
        if (p && strchr(p, '%')) {
            verr(v, line,
                 "format strings are forbidden: Pr takes values, not a format");
        }
    }
    buf_putc(&b, ')');
    emit_line(v, line, &b);
    free(b.s);
}

static void parse_prbits(V5 *v)
{
    int   line = cur(v)->line;
    Buf   val, out;
    Type *t;

    memset(&val, 0, sizeof val);
    memset(&out, 0, sizeof out);
    adv(v);
    expect_p(v, "(");
    t = expr(v, &val);
    expect_p(v, ")");
    if (v->failed) { free(val.s); return; }

    buf_puts(&out, "__lu_prbits(");
    buf_add(&out, val.s ? val.s : "0", val.len);
    if (v->last_bit_width > 0)
        buf_printf(&out, ", %d)", v->last_bit_width);
    else
        buf_printf(&out, ", %d)", type_bits(t));
    free(val.s);
    emit_line(v, line, &out);
    free(out.s);
}

static void parse_memset(V5 *v)
{
    int line = cur(v)->line;
    Buf b, a1, a2, a3;
    memset(&b, 0, sizeof b);
    memset(&a1, 0, sizeof a1);
    memset(&a2, 0, sizeof a2);
    memset(&a3, 0, sizeof a3);
    adv(v);
    expect_p(v, "(");
    expr(v, &a1); expect_p(v, ",");
    expr(v, &a2); expect_p(v, ",");
    expr(v, &a3); expect_p(v, ")");
    buf_printf(&b, "Memset/%s %s %s", a1.s, a2.s, a3.s);
    emit_line(v, line, &b);
    free(b.s); free(a1.s); free(a2.s); free(a3.s);
}

static void parse_memcpy(V5 *v)
{
    int line = cur(v)->line;
    Buf b, a1, a2, a3;
    memset(&b, 0, sizeof b);
    memset(&a1, 0, sizeof a1);
    memset(&a2, 0, sizeof a2);
    memset(&a3, 0, sizeof a3);
    adv(v);
    expect_p(v, "(");
    expr(v, &a1); expect_p(v, ",");
    expr(v, &a2); expect_p(v, ",");
    expr(v, &a3); expect_p(v, ")");
    buf_printf(&b, "Memcpy/%s %s %s", a1.s, a2.s, a3.s);
    emit_line(v, line, &b);
    free(b.s); free(a1.s); free(a2.s); free(a3.s);
}

/* ─────────────────────────── declarations ─────────────────────────────── */

static void parse_block_raw(V5 *v)
{
    int depth = 0;
    if (!at_p(v, "{")) {
        verr(v, cur(v)->line, "block required");
        return;
    }
    do {
        if (at_p(v, "{")) depth++;
        else if (at_p(v, "}")) depth--;
        adv(v);
    } while (depth > 0 && cur(v)->kind != TK_EOF);
}

/* ─────────────────────────── include files ─────────────────────────────── */

static bool file_readable(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static char *read_whole_file(const char *p)
{
    FILE   *f = fopen(p, "rb");
    long    n;
    size_t  got;
    char   *s;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    s = malloc((size_t)n + 1);
    if (!s) { fclose(f); return NULL; }
    got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = '\0';
    return s;
}

static char *path_dir(const char *p)
{
    char *d = xstrdup(p ? p : ".");
    char *slash = strrchr(d, '/');
    if (slash) *slash = '\0';
    else { free(d); d = xstrdup("."); }
    return d;
}

static char *path_join(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char  *s = malloc(n);
    if (!s) { fputs("out of memory\n", stderr); exit(1); }
    snprintf(s, n, "%s/%s", dir, name);
    return s;
}

static char *exe_dir(void)
{
    char    buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    return path_dir(buf);
}

/* Look for `name` (with .lu appended when no extension is given) next to the
   including file, then in ./include, ../include, $LU_INCLUDE and finally
   next to the compiler binary.  Returns a malloc'd path or NULL. */
static char *resolve_include(const char *from_file, const char *main_path,
                             const char *name)
{
    char *dirs[20];
    int   nd = 0, i;
    char *dir, *exe, *env, *found = NULL;
    char  ext[512];

    dir = path_dir(from_file ? from_file : main_path);
    dirs[nd++] = dir;
    dirs[nd++] = xstrdup(".");
    dirs[nd++] = xstrdup("include");
    dirs[nd++] = xstrdup("../include");

    exe = exe_dir();
    if (exe) {
        dirs[nd++] = path_join(exe, "include");
        dirs[nd++] = path_join(exe, "../include");
        free(exe);
    }

    env = getenv("LU_INCLUDE");
    if (env && *env) {
        char *copy = xstrdup(env), *p = copy, *q;
        while (p && *p && nd < 18) {
            q = strchr(p, ':');
            if (q) *q = '\0';
            if (*p) dirs[nd++] = xstrdup(p);
            p = q ? q + 1 : NULL;
        }
        free(copy);
    }

    snprintf(ext, sizeof ext, "%s.lu", name);

    for (i = 0; i < nd && !found; i++) {
        char *cand;
        size_t l = strlen(name);

        if (l > 3 && !strcmp(name + l - 3, ".lu")) {
            cand = path_join(dirs[i], name);
            if (file_readable(cand)) found = cand; else free(cand);
            continue;
        }
        cand = path_join(dirs[i], ext);
        if (file_readable(cand)) { found = cand; }
        else {
            free(cand);
            cand = path_join(dirs[i], name);
            if (file_readable(cand)) found = cand; else free(cand);
        }
    }

    for (i = 0; i < nd; i++) free(dirs[i]);
    return found;
}

static bool already_included(V5 *v, const char *path)
{
    int i;
    for (i = 0; i < v->ninc; i++)
        if (!strcmp(v->inc[i], path)) return true;
    return false;
}

static void remember_include(V5 *v, const char *path)
{
    if (v->ninc == v->cinc) {
        v->cinc = v->cinc ? v->cinc * 2 : 8;
        v->inc = realloc(v->inc, (size_t)v->cinc * sizeof *v->inc);
    }
    v->inc[v->ninc++] = xstrdup(path);
}

/* Insert `n` tokens starting at base[off] just after `incl <...>`; the
   whole array `base` is taken over (and released) by this call. */
static void splice_tokens(V5 *v, Tok *base, int off, int n)
{
    if (n > 0) {
        if (v->nt + n > v->nt_alloc) {
            while (v->nt_alloc < v->nt + n)
                v->nt_alloc = v->nt_alloc ? v->nt_alloc * 2 : 256;
            v->tk = realloc(v->tk, (size_t)v->nt_alloc * sizeof *v->tk);
        }
        memmove(&v->tk[v->pos + n], &v->tk[v->pos],
                (size_t)(v->nt - v->pos) * sizeof *v->tk);
        memcpy(&v->tk[v->pos], base + off, (size_t)n * sizeof *base);
        v->nt += n;
    }
    free(base);
}

static void parse_include(V5 *v)
{
    int         line = cur(v)->line;
    const char *from = cur(v)->file ? cur(v)->file : v->path;
    Buf         path;

    adv(v);
    memset(&path, 0, sizeof path);
    if (at_p(v, "<")) {
        adv(v);
        while (!v->failed && !at_p(v, ">") && cur(v)->kind != TK_EOF) {
            buf_puts(&path, cur(v)->text);
            adv(v);
        }
        expect_p(v, ">");
    } else if (cur(v)->kind == TK_STR) {
        buf_puts(&path, cur(v)->text);
        adv(v);
    } else {
        verr(v, line, "include path must be written as <name>");
        return;
    }
    if (v->failed) { free(path.s); return; }

    /* the std/ namespace is provided by the compiler itself */
    if (!strncmp(path.s ? path.s : "", "std/", 4)) { free(path.s); return; }

    /* C headers are not Lu source: pass the directive through to the v4 text
       instead of splicing the header into the token stream. */
    if (!strcmp(path.s ? path.s : "", "tensor.h")) {
        v->tensor_lib = true;
        if (!v->scan) {
            Buf directive;
            memset(&directive, 0, sizeof directive);
            buf_puts(&directive, "#include <tensor.h>");
            emit_line(v, line, &directive);
            free(directive.s);
        }
        free(path.s);
        return;
    }

    /* pass 1 pulls the file in once; pass 2 walks the tokens already spliced
       into the stream, so the directive itself only needs to be skipped. */
    if (v->scan) {
        char *full = resolve_include(from, v->path, path.s ? path.s : "");
        if (!full) {
            verr(v, line, "include file '%s' not found", path.s ? path.s : "");
        } else if (already_included(v, full)) {
            free(full);
        } else {
            char *src = read_whole_file(full);
            if (!src) {
                verr(v, line, "cannot read include file '%s'", full);
            } else {
                Tok *save_tk     = v->tk;
                int  save_nt     = v->nt;
                int  save_alloc  = v->nt_alloc;
                int  save_pos    = v->pos;
                const char *save_file = v->cur_file;
                int  first;

                v->tk = NULL; v->nt = 0; v->nt_alloc = 0; v->pos = 0;
                v->cur_file = full;
                lex_all(v, src);
                free(src);

                first = (v->nt >= 3 &&
                         !strcmp(v->tk[0].text, "Lu") &&
                         !strcmp(v->tk[1].text, "/") &&
                         !strcmp(v->tk[2].text, "Strict")) ? 3 : 0;
                if (first == 0 && !v->failed) {
                    v->err[0] = '\0';
                    v->failed = true;
                    snprintf(v->err, sizeof v->err,
                             "%s:1: error: first line must be 'Lu/Strict'", full);
                }

                {
                    Tok *add   = v->tk;
                    int  nadd  = v->nt - 1;   /* keep the main file's EOF */
                    int  off   = first;

                    v->tk = save_tk;
                    v->nt = save_nt;
                    v->nt_alloc = save_alloc;
                    v->pos = save_pos;
                    v->cur_file = save_file;

                    if (!v->failed)
                        splice_tokens(v, add, off, nadd - off);
                    else
                        free(add);
                }
                remember_include(v, full);
            }
            free(full);
        }
    }
    free(path.s);
}

static void emit_struct(V5 *v, Struct *s)
{
    int i;
    buf_printf(&v->out, "struct %s {\n", s->name);
    v->out_line++;
    for (i = 0; i < s->nf; i++) {
        Buf ft;
        memset(&ft, 0, sizeof ft);
        type_text(s->f[i].ty, &ft);
        buf_printf(&v->out, "    %s: %s\n", s->f[i].name, ft.s);
        v->out_line++;
        free(ft.s);
    }
    buf_puts(&v->out, "}\n");
    v->out_line++;
}

static void parse_struct(V5 *v)
{
    int line = cur(v)->line;
    Tok *nt;
    Struct *s;

    adv(v); /* struct */
    nt = expect_id(v, "a struct name");
    if (v->failed) return;
    s = v->scan ? new_struct(v, nt->text) : find_struct(v, nt->text);
    if (!s) { verr(v, line, "internal: struct '%s' not scanned", nt->text); return; }
    expect_p(v, "{");

    while (!v->failed && !at_p(v, "}") && cur(v)->kind != TK_EOF) {
        if (at_id(v, "fn")) {
            /* method signature */
            Tok *mn;
            Fn  *m;
            adv(v);
            mn = expect_id(v, "a method name");
            if (v->failed) return;
            if (v->scan) {
                if (s->nm == s->cm) {
                    s->cm = s->cm ? s->cm * 2 : 4;
                    s->m = realloc(s->m, (size_t)s->cm * sizeof *s->m);
                }
                m = &s->m[s->nm++];
                memset(m, 0, sizeof *m);
                m->name = mn->text;
                m->ret  = &ty_void;
                m->self = true;
            } else {
                m = struct_method(s, mn->text);
                if (!m) { verr(v, mn->line, "internal: method not scanned"); return; }
            }
            if (!v->scan) m->np = 0;   /* re-scan of the same signature */
            expect_p(v, "(");
            while (!at_p(v, ")")) {
                Tok *pn = expect_id(v, "a parameter name");
                Type *pt;
                if (v->failed) return;
                if (at_p(v, ":")) { adv(v); pt = parse_type(v); }
                else if (!strcmp(pn->text, "self"))
                    pt = mk_ptr(mk_type(TY_NAMED, s->name, NULL, 0));
                else { verr(v, pn->line, "missing type for '%s'", pn->text); return; }
                if (m->np < 6) fn_param(m, pn->text, pt);
                if (at_p(v, ",")) adv(v); else break;
            }
            expect_p(v, ")");
            if (at_p(v, "->")) { adv(v); m->ret = parse_type(v); }
            if (v->scan) { parse_block_raw(v); continue; }

            /* emit */
            {
                Buf head;
                int i;
                memset(&head, 0, sizeof head);
                buf_printf(&head, "fn %s__%s(", s->name, mn->text);
                for (i = 0; i < m->np; i++) {
                    Buf pt;
                    memset(&pt, 0, sizeof pt);
                    type_text(m->ptype[i], &pt);
                    if (i) buf_puts(&head, ", ");
                    buf_printf(&head, "%s: %s", m->pname[i], pt.s);
                    free(pt.s);
                }
                buf_puts(&head, ") -> ");
                type_text(m->ret, &head);
                open_block(v, &head, line);
                free(head.s);

                v->nloc = 0;
                {
                    int has_self = 0;
                    for (i = 0; i < m->np; i++)
                        if (!strcmp(m->pname[i], "self")) has_self = 1;
                    if (!has_self)
                        push_var(v, xstrdup("self"),
                                 mk_ptr(mk_type(TY_NAMED, s->name, NULL, 0)),
                                 true, false);
                }
                for (i = 0; i < m->np; i++)
                    push_var(v, m->pname[i], m->ptype[i], true, false);
                body(v);
                close_block(v);
                v->nloc = 0;
            }
            continue;
        }

        {
            bool is_pub = false;
            Tok *fn_tok;
            Type *ft;
            if (at_id(v, "pub")) { is_pub = true; adv(v); }
            fn_tok = expect_id(v, "a field name");
            if (v->failed) return;
            expect_p(v, ":");
            ft = parse_type(v);
            (void)is_pub;
            if (v->scan) {
                if (s->nf == s->cf) {
                    s->cf = s->cf ? s->cf * 2 : 4;
                    s->f = realloc(s->f, (size_t)s->cf * sizeof *s->f);
                }
                s->f[s->nf].name = fn_tok->text;
                s->f[s->nf].ty   = ft;
                s->nf++;
            }
        }
    }
    expect_p(v, "}");

    if (!v->scan) emit_struct(v, s);
    (void)line;
}

static void parse_pack(V5 *v)
{
    int line = cur(v)->line;
    Tok *unit, *nt;
    Pack *pk;

    adv(v); /* pack */
    unit = expect_id(v, "a storage unit (byte)");
    (void)unit;
    nt = expect_id(v, "a pack name");
    if (v->failed) return;

    if (v->scan) {
        if (v->npk == v->cpk) {
            v->cpk = v->cpk ? v->cpk * 2 : 4;
            v->pks = realloc(v->pks, (size_t)v->cpk * sizeof *v->pks);
        }
        pk = &v->pks[v->npk++];
        memset(pk, 0, sizeof *pk);
        pk->name = nt->text;
    } else {
        pk = find_pack(v, nt->text);
        if (!pk) { verr(v, line, "internal: pack not scanned"); return; }
    }

    expect_p(v, "{");
    while (!v->failed && !at_p(v, "}") && cur(v)->kind != TK_EOF) {
        Tok *bt = expect_id(v, "a bit range (vid0..N)");
        int  shift = 0, end = 0, width;
        Tok *ft;
        Type *ftype;

        if (v->failed) return;
        if (strncmp(bt->text, "vid", 3) || !isdigit((unsigned char)bt->text[3])) {
            verr(v, bt->line, "bit field must start with vid<N>");
            return;
        }
        shift = atoi(bt->text + 3);
        expect_p(v, "..");
        if (cur(v)->kind == TK_NUM) end = (int)cur(v)->num;
        else if (cur(v)->kind == TK_ID) end = atoi(cur(v)->text);
        else { verr(v, cur(v)->line, "expected a bit index"); return; }
        adv(v);
        ft = expect_id(v, "a field name");
        if (v->failed) return;
        expect_p(v, ":");
        ftype = parse_type(v);
        if (at_p(v, "=")) {
            Buf junk;
            memset(&junk, 0, sizeof junk);
            adv(v);
            expr(v, &junk);
            free(junk.s);
        }
        width = end - shift + 1;
        if (width <= 0) {
            verr(v, bt->line, "invalid bit range %d..%d", shift, end);
            return;
        }
        if (v->scan) {
            if (pk->nb == pk->cap) {
                pk->cap = pk->cap ? pk->cap * 2 : 4;
                pk->b = realloc(pk->b, (size_t)pk->cap * sizeof *pk->b);
            }
            pk->b[pk->nb].name  = ft->text;
            pk->b[pk->nb].shift = shift;
            pk->b[pk->nb].width = width;
            pk->b[pk->nb].ty    = ftype;
            pk->nb++;
        }
    }
    expect_p(v, "}");

    if (!v->scan) {
        buf_printf(&v->out, "struct %s {\n    bits: int\n}\n", pk->name);
        v->out_line += 3;
    }
}

static void parse_fn(V5 *v)
{
    int line = cur(v)->line;
    Tok *nt;
    Fn  *f;
    int  i;

    adv(v); /* fn */
    nt = expect_id(v, "a function name");
    if (v->failed) return;

    if (v->scan) {
        f = new_fn(v, nt->text);
    } else {
        f = find_fn(v, nt->text);
        if (!f) { verr(v, line, "internal: function not scanned"); return; }
    }

    if (!v->scan) f->np = 0;   /* re-scan of the same signature */

    expect_p(v, "(");
    while (!v->failed && !at_p(v, ")")) {
        Tok *pn = expect_id(v, "a parameter name");
        Type *pt;
        if (v->failed) return;
        if (!at_p(v, ":")) {
            verr(v, pn->line, "missing type for '%s'", pn->text);
            return;
        }
        adv(v);
        pt = parse_type(v);
        if (f->np < 6) fn_param(f, pn->text, pt);
        else verr(v, pn->line, "too many parameters (max 6)");
        if (at_p(v, ",")) { adv(v); continue; }
        break;
    }
    expect_p(v, ")");
    if (at_p(v, "->")) { adv(v); f->ret = parse_type(v); }

    if (v->scan) { parse_block_raw(v); return; }

    {
        Buf head;
        memset(&head, 0, sizeof head);
        buf_puts(&head, "fn ");
        buf_puts(&head, f->name);
        buf_putc(&head, '(');
        for (i = 0; i < f->np; i++) {
            Buf pt;
            memset(&pt, 0, sizeof pt);
            type_text(f->ptype[i], &pt);
            if (i) buf_puts(&head, ", ");
            buf_printf(&head, "%s: %s", f->pname[i], pt.s);
            free(pt.s);
        }
        buf_puts(&head, ") -> ");
        type_text(f->ret, &head);
        open_block(v, &head, line);

        v->nloc = 0;
        for (i = 0; i < f->np; i++)
            push_var(v, f->pname[i], f->ptype[i], true, false);
        body(v);
        close_block(v);
        v->nloc = 0;
    }
}

/* ─────────────────────────── program ───────────────────────────────────── */

static void parse_global(V5 *v)
{
    int   line = cur(v)->line;
    Tok  *kt = cur(v);
    bool  is_vid = !strcmp(kt->text, "vid");
    Tok  *nt;
    Type *ty;
    Buf   decl;

    if (!is_vid) {
        verr(v, line, "global state must be declared immutable with 'vid'");
        return;
    }
    adv(v);
    nt = expect_id(v, "a name");
    if (v->failed) return;
    if (!at_p(v, ":")) { verr(v, nt->line, "missing type for '%s'", nt->text); return; }
    adv(v);
    ty = parse_type(v);
    if (v->failed) return;

    memset(&decl, 0, sizeof decl);
    buf_printf(&decl, "var %s: ", nt->text);
    type_text(ty, &decl);
    if (at_p(v, "=")) {
        Buf init;
        Type *it;
        memset(&init, 0, sizeof init);
        adv(v);
        it = expr(v, &init);
        if (!compat(ty, it))
            verr(v, line, "cannot initialise '%s' with %s",
                 nt->text, kind_name(it->kind));
        buf_puts(&decl, " = ");
        buf_add(&decl, init.s ? init.s : "", init.len);
        free(init.s);
    } else if (ty->kind == TY_STR) {
        buf_puts(&decl, " = \"\"");
    }
    if (!v->scan) emit_line(v, line, &decl);
    free(decl.s);
    if (v->scan) push_var(v, nt->text, ty, true, true);
}

static void parse_program(V5 *v)
{
    while (!v->failed && cur(v)->kind != TK_EOF) {
        if (at_id(v, "incl"))        { parse_include(v); continue; }
        if (at_id(v, "vid"))         { parse_global(v);  continue; }
        if (at_id(v, "var")) {
            verr(v, cur(v)->line, "global state must be declared immutable with 'vid'");
            return;
        }
        if (at_id(v, "struct"))      { parse_struct(v);  continue; }
        if (at_id(v, "pack"))        { parse_pack(v);    continue; }
        if (at_id(v, "fn"))          { parse_fn(v);      continue; }
        if (at_p(v, "}")) { adv(v); continue; }
        verr(v, cur(v)->line, "unexpected token '%s' at top level", cur(v)->text);
    }
}

/* ─────────────────────────── prelude ───────────────────────────────────── */

static const char *V5_PRELUDE =
    "\n"
    "fn __lu_b2i(b: bool) -> int {\n"
    "    if b { return 1 }\n"
    "    return 0\n"
    "}\n"
    "fn __lu_pkget(bits: int, shift: int, width: int) -> int {\n"
    "    var mask: int = (1 << width) - 1\n"
    "    return (bits >> shift) & mask\n"
    "}\n"
    "fn __lu_pkset(bits: int, shift: int, width: int, val: int) -> int {\n"
    "    var mask: int = ((1 << width) - 1) << shift\n"
    "    return (bits & (~mask)) | ((val << shift) & mask)\n"
    "}\n"
    "fn __lu_streq(a: str, b: str) -> bool {\n"
    "    var i: int = 0\n"
    "    loop 4096 {\n"
    "        if a[i] != b[i] { return false }\n"
    "        if a[i] == 0 { return true }\n"
    "        i = i + 1\n"
    "    }\n"
    "    return false\n"
    "}\n"
    "fn __lu_prbits(val: int, width: int) -> void {\n"
    "    var buf: byte[80]\n"
    "    var i: int = 0\n"
    "    var k: int = width - 1\n"
    "    loop 80 {\n"
    "        if k < 0 { break }\n"
    "        if ((val >> k) & 1) != 0 { buf[i] = 49 } else { buf[i] = 48 }\n"
    "        i = i + 1\n"
    "        k = k - 1\n"
    "    }\n"
    "    buf[i] = 0\n"
    "    print(buf)\n"
    "}\n";

/* ─────────────────────────── entry point ───────────────────────────────── */

int v5_is_strict(const char *src)
{
    const char *p = src;
    if (!p) return 0;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        const char *e;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        e = p;
        while (*e && *e != '\n' && *e != ' ' && *e != '\t') e++;
        return (e - p >= 9) && !strncmp(p, "Lu/Strict", 9);
    }
    return 0;
}

char *v5_transpile(const char *path, const char *src, char *err, size_t errcap)
{
    V5    v;
    char *slash;

    memset(&v, 0, sizeof v);
    v.path     = path;
    v.cur_file = path;
    v.out_line = 1;
    v.tmp      = 0;

    v.dir = xstrdup(path);
    slash = strrchr(v.dir, '/');
    if (slash) *slash = '\0';
    else { free(v.dir); v.dir = xstrdup("."); }

    lex_all(&v, src);
    if (v.failed) goto fail;

    if (v.nt < 4 ||
        strcmp(v.tk[0].text, "Lu") || strcmp(v.tk[1].text, "/") ||
        strcmp(v.tk[2].text, "Strict")) {
        snprintf(v.err, sizeof v.err,
                 "%s:1: error: first line must be 'Lu/Strict'", path);
        v.failed = true;
        goto fail;
    }
    v.pos = 3;

    /* pass 1: signatures */
    v.scan = true;
    parse_program(&v);
    if (v.failed) goto fail;

    /* pass 2: emit */
    v.scan     = false;
    v.pos      = 3;
    v.out_line = 1;
    v.out.len  = 0;
    if (v.out.s) v.out.s[0] = '\0';
    v.depth = 0;
    v.nloc  = 0;
    v.nloop = 0;
    parse_program(&v);
    if (v.failed) goto fail;

    buf_puts(&v.out, V5_PRELUDE);

    free(v.dir);
    return v.out.s;

fail:
    if (err && errcap) snprintf(err, errcap, "%s", v.err);
    return NULL;
}
