/* ══════════════════════════════════════════════════════════════════
   x86.c — native x86-64 backend for Lu

   Lowers the Lu AST straight to machine code and writes a static
   ELF64 executable for Linux. No libc, no gcc, no assembler, no
   linker: instruction bytes are emitted directly, the runtime is
   written in Lu itself (see the prelude below) and talks to the
   kernel through raw syscalls.

   Scope: the low-level core of the language. Classes, generics,
   floats, f-strings with runtime parts, X11 and tensors still go
   through the C backend (`luc -c`).
   ══════════════════════════════════════════════════════════════════ */

#include "x86.h"
#include <stdarg.h>
#include <sys/stat.h>

/* ─────────────────────────────────────────────
   Layout constants
   ───────────────────────────────────────────── */
#define XV_PAGE       4096
#define XV_BASE       0x400000ULL
#define XV_CODE_OFF   0x1000
#define XV_CODE_VA    (XV_BASE + XV_CODE_OFF)
#define XV_MAX_TEMP   32
#define XV_MAX_LABEL  16384
#define XV_MAX_FIX    65536
#define XV_MAX_ERR    20

/* ─────────────────────────────────────────────
   Growable byte buffer
   ───────────────────────────────────────────── */
typedef struct { unsigned char *p; int len, cap; } Buf;

static void buf_put(Buf *b, const void *d, int n) {
    if (n <= 0) return;
    if (b->len + n > b->cap) {
        int nc = b->cap ? b->cap : 1024;
        while (nc < b->len + n) nc *= 2;
        unsigned char *np = realloc(b->p, (size_t)nc);
        if (!np) { fputs("luc: out of memory\n", stderr); exit(1); }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, d, (size_t)n);
    b->len += n;
}
static void buf_u8(Buf *b, int v)  { unsigned char c = (unsigned char)v; buf_put(b, &c, 1); }
static void buf_i32(Buf *b, int32_t v) { buf_put(b, &v, 4); }
static void buf_i64(Buf *b, int64_t v) { buf_put(b, &v, 8); }

/* ─────────────────────────────────────────────
   Types
   ───────────────────────────────────────────── */
typedef enum {
    TY_VOID, TY_INT, TY_I64, TY_BYTE, TY_BOOL, TY_STR, TY_PTR, TY_STRUCT, TY_FLOAT
} TyKind;

typedef struct Type Type;

typedef struct {
    char  name[64];
    int   off;
    Type *ty;
    int   count;      /* >0: inline array field */
} Field;

struct Type {
    TyKind kind;
    int    size;
    Type  *base;      /* pointee / element type */
    char   name[64];
    int    nfields;
    Field  fields[32];
};

static Type ty_void, ty_int, ty_i64, ty_byte, ty_bool, ty_str, ty_float;

static Type mk_prim(TyKind k, int sz, const char *nm) {
    Type t;
    memset(&t, 0, sizeof t);
    t.kind = k;
    t.size = sz;
    snprintf(t.name, sizeof t.name, "%s", nm);
    return t;
}

static void types_init(void) {
    ty_void  = mk_prim(TY_VOID, 0, "void");
    ty_int   = mk_prim(TY_INT, 4, "int");
    ty_i64   = mk_prim(TY_I64, 8, "int64");
    ty_byte  = mk_prim(TY_BYTE, 1, "byte");
    ty_bool  = mk_prim(TY_BOOL, 1, "bool");
    ty_str   = mk_prim(TY_STR, 8, "str");
    ty_float = mk_prim(TY_FLOAT, 4, "float");
}

#define XV_MAX_STRUCT 128
static Type *g_structs[XV_MAX_STRUCT];
static int   g_nstruct;

static Type *mk_ptr(Type *base) {
    Type *t = calloc(1, sizeof(Type));
    if (!t) { fputs("luc: out of memory\n", stderr); exit(1); }
    t->kind = TY_PTR;
    t->size = 8;
    t->base = base;
    /* Type names are display-only. Truncate the pointee rather than the
       trailing '*', so a deep pointer chain still reads as a pointer in
       diagnostics instead of silently losing its marker. */
    {
        const char *inner = base ? base->name : "void";
        size_t n = strlen(inner);
        if (n > sizeof t->name - 2) n = sizeof t->name - 2;
        memcpy(t->name, inner, n);
        t->name[n] = '*';
        t->name[n + 1] = '\0';
    }
    return t;
}

/* ─────────────────────────────────────────────
   Symbols
   ───────────────────────────────────────────── */
typedef struct {
    char  name[128];
    Type *ty;        /* value type; for arrays the element type */
    bool  is_array;
    int   count;
    int   off;       /* local: rbp-off; global: data offset */
    bool  global;
} Var;

#define XV_MAX_VAR 2048
static Var g_locals[XV_MAX_VAR];
static int g_nlocals;
static int g_scope[512];
static int g_nscope;

static Var g_globals[512];
static int g_nglobals;

typedef struct {
    char  name[128];
    Type *ret;
    int   nparams;
    Type *params[8];
    int   label;
    bool  defined;
} Fn;

static Fn g_fns[256];
static int g_nfns;

typedef struct { char name[64]; int64_t val; } EnumConst;
static EnumConst g_enums[256];
static int g_nenums;

/* ─────────────────────────────────────────────
   Codegen state
   ───────────────────────────────────────────── */
typedef struct {
    Buf code, data;

    int  label_pos[XV_MAX_LABEL];
    int  nlabels;

    struct { int at, lbl; } cfix[XV_MAX_FIX];
    int  ncfix;
    struct { int at, doff; } dfix[XV_MAX_FIX];
    int  ndfix;
    /* data -> data fixups: patch an 8-byte slot inside .data with the
       absolute address of another .data offset (global pointer initialisers) */
    struct { int at, doff; } gdfix[XV_MAX_FIX];
    int  ngdfix;

    int  frame;
    int  locals_used;
    int  temp_depth;
    int  brk[64], cont[64], nctrl;
    int  epilogue;

    bool failed;
    int  nerr;
    const char *src;
} X;

/* ─────────────────────────────────────────────
   Diagnostics
   ───────────────────────────────────────────── */
static void xfail(X *x, int line, const char *fmt, ...) {
    x->failed = true;
    if (x->nerr >= XV_MAX_ERR) return;
    x->nerr++;
    fprintf(stderr, "%s:%d: error: ", x->src ? x->src : "<native>", line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (x->nerr == 1)
        fprintf(stderr, "  (the native backend covers the low-level core; "
                        "use `luc -c` for the full language)\n");
}

/* ─────────────────────────────────────────────
   Instruction encoder
   ───────────────────────────────────────────── */
static void e8(X *x, int v)  { buf_u8(&x->code, v); }
static void e32(X *x, int32_t v) { buf_i32(&x->code, v); }
static void e64(X *x, int64_t v) { buf_i64(&x->code, v); }

/* REX with W=1 forced */
static void rex_w(X *x, int r, int b) {
    e8(x, 0x48 | ((r & 8) ? 4 : 0) | ((b & 8) ? 1 : 0));
}
/* REX only when a high register is used */
static void rex_nw(X *x, int r, int b) {
    int v = 0x40 | ((r & 8) ? 4 : 0) | ((b & 8) ? 1 : 0);
    if (v != 0x40) e8(x, v);
}
/* mod=10, rm=base, disp32 */
static void modrm_mem(X *x, int reg, int base, int disp) {
    e8(x, 0x80 | ((reg & 7) << 3) | (base & 7));
    e32(x, disp);
}
static void modrm_rr(X *x, int reg, int rm) {
    e8(x, 0xC0 | ((reg & 7) << 3) | (rm & 7));
}

/* mov rax, imm64 */
static void i_mov_rax_imm(X *x, int64_t v) {
    if (v >= -2147483648LL && v <= 2147483647LL) {
        e8(x, 0x48); e8(x, 0xC7); e8(x, 0xC0); e32(x, (int32_t)v);
    } else {
        e8(x, 0x48); e8(x, 0xB8); e64(x, v);
    }
}
/* mov r64, imm32 (sign-extended) */
static void i_mov_reg_imm(X *x, int reg, int64_t v) {
    if (reg == 0) { i_mov_rax_imm(x, v); return; }
    rex_w(x, 0, reg);
    e8(x, 0xC7);
    modrm_rr(x, 0, reg);
    e32(x, (int32_t)v);
}
/* mov dst, src (64-bit) */
static void i_mov_reg_reg(X *x, int dst, int src) {
    rex_w(x, src, dst);
    e8(x, 0x89);
    modrm_rr(x, src, dst);
}
/* mov reg, [rbp+disp] */
static void i_mov_reg_mem(X *x, int reg, int disp) {
    rex_w(x, reg, 5);
    e8(x, 0x8B);
    modrm_mem(x, reg, 5, disp);
}
/* mov [rbp+disp], reg */
static void i_mov_mem_reg(X *x, int disp, int reg) {
    rex_w(x, reg, 5);
    e8(x, 0x89);
    modrm_mem(x, reg, 5, disp);
}
/* lea reg, [rbp-off]

   Local variables and arrays are addressed by their positive distance
   below rbp; these helpers flip it to the negative displacement the
   instruction actually encodes. Temporaries use their own negative
   displacements directly through i_mov_reg_mem/i_mov_mem_reg. */
static void i_lea_mem(X *x, int reg, int disp) {
    rex_w(x, reg, 5);
    e8(x, 0x8D);
    modrm_mem(x, reg, 5, -disp);
}

/* typed load: rax = [rbp-off] */
static void i_load_slot(X *x, int disp, Type *t) {
    if (t->kind == TY_BOOL || t->kind == TY_BYTE) {
        e8(x, 0x0F); e8(x, 0xB6); modrm_mem(x, 0, 5, -disp);
    } else if (t->size == 8) {
        rex_w(x, 0, 5); e8(x, 0x8B); modrm_mem(x, 0, 5, -disp);
    } else {
        rex_w(x, 0, 5); e8(x, 0x63); modrm_mem(x, 0, 5, -disp);
    }
}
/* typed store: [rbp-off] = rax (byte/bool widened to 32 bits) */
static void i_store_slot(X *x, int disp, Type *t) {
    if (t->kind == TY_BOOL || t->kind == TY_BYTE) {
        e8(x, 0x89); modrm_mem(x, 0, 5, -disp);
    } else if (t->size == 8) {
        rex_w(x, 0, 5); e8(x, 0x89); modrm_mem(x, 0, 5, -disp);
    } else {
        e8(x, 0x89); modrm_mem(x, 0, 5, -disp);
    }
}
/* typed store from an arbitrary register */
static void i_store_slot_reg(X *x, int disp, Type *t, int reg) {
    if (t->kind == TY_BOOL || t->kind == TY_BYTE || t->size == 4) {
        rex_nw(x, reg, 5);
        e8(x, 0x89);
        modrm_mem(x, reg, 5, -disp);
    } else {
        rex_w(x, reg, 5);
        e8(x, 0x89);
        modrm_mem(x, reg, 5, -disp);
    }
}

/* rax = [rax] with the given element type */
static void i_load_at_rax(X *x, Type *t) {
    if (t->kind == TY_BOOL || t->kind == TY_BYTE) {
        e8(x, 0x0F); e8(x, 0xB6); e8(x, 0x00);
    } else if (t->size == 8) {
        e8(x, 0x48); e8(x, 0x8B); e8(x, 0x00);
    } else {
        e8(x, 0x48); e8(x, 0x63); e8(x, 0x00);
    }
}
/* [rcx] = rax */
static void i_store_at_rcx(X *x, Type *t) {
    if (t->kind == TY_BOOL || t->kind == TY_BYTE) {
        e8(x, 0x88); e8(x, 0x01);
    } else if (t->size == 8) {
        e8(x, 0x48); e8(x, 0x89); e8(x, 0x01);
    } else {
        e8(x, 0x89); e8(x, 0x01);
    }
}

/* arithmetic rax ← rax OP rcx */
static void i_add(X *x)  { e8(x, 0x48); e8(x, 0x01); e8(x, 0xC8); }
static void i_sub(X *x)  { e8(x, 0x48); e8(x, 0x29); e8(x, 0xC8); }
static void i_imul(X *x) { e8(x, 0x48); e8(x, 0x0F); e8(x, 0xAF); e8(x, 0xC1); }
static void i_and(X *x)  { e8(x, 0x48); e8(x, 0x21); e8(x, 0xC8); }
static void i_or(X *x)   { e8(x, 0x48); e8(x, 0x09); e8(x, 0xC8); }
static void i_xor(X *x)  { e8(x, 0x48); e8(x, 0x31); e8(x, 0xC8); }
static void i_neg(X *x)  { e8(x, 0x48); e8(x, 0xF7); e8(x, 0xD8); }
static void i_not(X *x)  { e8(x, 0x48); e8(x, 0xF7); e8(x, 0xD0); }
static void i_dec_rax(X *x) { e8(x, 0x48); e8(x, 0xFF); e8(x, 0xC8); }
static void i_test_rax(X *x) { e8(x, 0x48); e8(x, 0x85); e8(x, 0xC0); }
/* rax = rax / rcx, rax = rax % rcx */
static void i_idiv(X *x) { e8(x, 0x48); e8(x, 0x99); e8(x, 0x48); e8(x, 0xF7); e8(x, 0xF9); }
static void i_mod_rax(X *x) { e8(x, 0x48); e8(x, 0x89); e8(x, 0xD0); }
/* shifts, count in cl */
static void i_shl(X *x) { e8(x, 0x48); e8(x, 0xD3); e8(x, 0xE0); }
static void i_sar(X *x) { e8(x, 0x48); e8(x, 0xD3); e8(x, 0xF8); }
static void i_shr(X *x) { e8(x, 0x48); e8(x, 0xD3); e8(x, 0xE8); }
/* `cmp rax, rcx` — flags reflect `rax OP rcx`, i.e. `lhs OP rhs` given the
   convention that lhs lives in rax and rhs in rcx. Keep the operand order
   this way round: swapping it inverts every signed relational operator. */
static void i_cmp_rax_rcx(X *x) { e8(x, 0x48); e8(x, 0x39); e8(x, 0xC8); }
static void i_setcc(X *x, int cc) {
    e8(x, 0x0F); e8(x, 0x90 | cc); e8(x, 0xC0);
    e8(x, 0x0F); e8(x, 0xB6); e8(x, 0xC0);
}
static void i_movzx_rax_al(X *x) { e8(x, 0x0F); e8(x, 0xB6); e8(x, 0xC0); }
static void i_movsxd_rax_eax(X *x) { e8(x, 0x48); e8(x, 0x63); e8(x, 0xC0); }

/* condition codes */
enum { CC_E = 4, CC_NE = 5, CC_B = 2, CC_AE = 3, CC_L = 0xC, CC_GE = 0xD,
       CC_LE = 0xE, CC_G = 0xF };

/* ─────────────────────────────────────────────
   Labels and fixups
   ───────────────────────────────────────────── */
static int new_label(X *x) {
    if (x->nlabels >= XV_MAX_LABEL) {
        xfail(x, 0, "program too complex: label table exhausted");
        return 0;
    }
    return x->nlabels++;
}
static void place_label(X *x, int l) {
    if (l >= 0 && l < XV_MAX_LABEL) x->label_pos[l] = x->code.len;
}
static void add_cfix(X *x, int at, int lbl) {
    if (x->ncfix >= XV_MAX_FIX) {
        xfail(x, 0, "program too large: relocation table exhausted");
        return;
    }
    x->cfix[x->ncfix].at  = at;
    x->cfix[x->ncfix].lbl = lbl;
    x->ncfix++;
}
static void add_dfx(X *x, int at, int doff) {
    if (x->ndfix >= XV_MAX_FIX) {
        xfail(x, 0, "program too large: data relocations exhausted");
        return;
    }
    x->dfix[x->ndfix].at   = at;
    x->dfix[x->ndfix].doff = doff;
    x->ndfix++;
}
/* Patch the 8-byte slot at data offset `at` with the absolute address of
   data offset `doff`. Used for global pointers/strings initialised at
   compile time, where the address is only known after layout. */
static void add_gdfx(X *x, int at, int doff) {
    if (x->ngdfix >= XV_MAX_FIX) {
        xfail(x, 0, "program too large: data relocations exhausted");
        return;
    }
    x->gdfix[x->ngdfix].at   = at;
    x->gdfix[x->ngdfix].doff = doff;
    x->ngdfix++;
}static void i_jmp(X *x, int l) {
    e8(x, 0xE9);
    int at = x->code.len; e32(x, 0);
    add_cfix(x, at, l);
}
static void i_jcc(X *x, int cc, int l) {
    e8(x, 0x0F); e8(x, 0x80 | cc);
    int at = x->code.len; e32(x, 0);
    add_cfix(x, at, l);
}
static void i_call(X *x, int l) {
    e8(x, 0xE8);
    int at = x->code.len; e32(x, 0);
    add_cfix(x, at, l);
}
static void i_syscall(X *x) { e8(x, 0x0F); e8(x, 0x05); }

/* lea rax, [rip + data offset] */
static void i_lea_data(X *x, int doff) {
    e8(x, 0x48); e8(x, 0x8D); e8(x, 0x05);
    int at = x->code.len; e32(x, 0);
    add_dfx(x, at, doff);
}

/* ─────────────────────────────────────────────
   Data section helpers
   ───────────────────────────────────────────── */
static int data_align(X *x, int n) {
    while (x->data.len % n) buf_u8(&x->data, 0);
    return x->data.len;
}
static int data_alloc(X *x, int n) {
    int off = data_align(x, 8);
    for (int i = 0; i < n; i++) buf_u8(&x->data, 0);
    return off;
}
static int data_bytes(X *x, const char *s, int n) {
    int off = x->data.len;
    for (int i = 0; i < n; i++) buf_u8(&x->data, (unsigned char)s[i]);
    return off;
}
static int data_str(X *x, const char *s) {
    int off = data_bytes(x, s, (int)strlen(s));
    buf_u8(&x->data, 0);
    return off;
}

/* ─────────────────────────────────────────────
   Small utilities
   ───────────────────────────────────────────── */
static int align8(int n)  { return (n + 7) & ~7; }
static int align16(int n) { return (n + 15) & ~15; }

static Var *find_var(const char *name) {
    if (!name) return NULL;
    for (int i = g_nlocals - 1; i >= 0; i--)
        if (!strcmp(g_locals[i].name, name)) return &g_locals[i];
    for (int i = 0; i < g_nglobals; i++)
        if (!strcmp(g_globals[i].name, name)) return &g_globals[i];
    return NULL;
}
static Fn *find_fn(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_nfns; i++)
        if (!strcmp(g_fns[i].name, name)) return &g_fns[i];
    return NULL;
}
static Fn *add_fn(X *x, const char *name) {
    Fn *f = find_fn(name);
    if (f) return f;
    if (g_nfns >= 256) { xfail(x, 0, "too many functions"); return &g_fns[0]; }
    f = &g_fns[g_nfns++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name ? name : "_fn");
    f->ret = &ty_int;
    f->label = new_label(x);
    return f;
}
static bool find_enum(const char *name, int64_t *out) {
    for (int i = 0; i < g_nenums; i++)
        if (!strcmp(g_enums[i].name, name)) { if (out) *out = g_enums[i].val; return true; }
    return false;
}
static void add_enum(X *x, const char *name, int64_t v) {
    if (g_nenums >= 256) { xfail(x, 0, "too many enum constants"); return; }
    snprintf(g_enums[g_nenums].name, sizeof g_enums[0].name, "%s", name);
    g_enums[g_nenums].val = v;
    g_nenums++;
}
static bool is_byte_like(Type *t) { return t && (t->kind == TY_BYTE || t->kind == TY_BOOL); }
static bool is_ptr_like(Type *t) { return t && (t->kind == TY_PTR || t->kind == TY_STR); }

static void scope_push(void) {
    if (g_nscope < 512) g_scope[g_nscope++] = g_nlocals;
}
static void scope_pop(void) {
    if (g_nscope > 0) g_nlocals = g_scope[--g_nscope];
}
static void push_var(const char *name, Type *ty, bool is_array, int count, int off, bool global) {
    if (g_nlocals >= XV_MAX_VAR) return;
    Var *v = &g_locals[g_nlocals++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name ? name : "_v");
    v->ty = ty;
    v->is_array = is_array;
    v->count = count;
    v->off = off;
    v->global = global;
}

/* ─────────────────────────────────────────────
   Types from source names
   ───────────────────────────────────────────── */
static Type *type_from_name(X *x, const char *n) {
    if (!n || !*n) return &ty_int;
    if (!strcmp(n, "int")) return &ty_int;
    if (!strcmp(n, "int64") || !strcmp(n, "long") || !strcmp(n, "u64")) return &ty_i64;
    if (!strcmp(n, "byte") || !strcmp(n, "char") || !strcmp(n, "u8")) return &ty_byte;
    if (!strcmp(n, "bool")) return &ty_bool;
    if (!strcmp(n, "str") || !strcmp(n, "string") || !strcmp(n, "id")) return &ty_str;
    if (!strcmp(n, "void")) return &ty_void;
    if (!strcmp(n, "float") || !strcmp(n, "double") || !strcmp(n, "f32") || !strcmp(n, "f64"))
        return &ty_float;
    if (!strncmp(n, "ptr/", 4)) return mk_ptr(type_from_name(x, n + 4));
    size_t L = strlen(n);
    if (L > 1 && n[L - 1] == '*') {
        char b[128];
        size_t k = L - 1;
        if (k >= sizeof b) k = sizeof b - 1;
        memcpy(b, n, k);
        b[k] = '\0';
        return mk_ptr(type_from_name(x, b));
    }
    if (n[0] == '?' || n[0] == '!') return type_from_name(x, n + 1);
    for (int i = 0; i < g_nstruct; i++)
        if (!strcmp(g_structs[i]->name, n)) return g_structs[i];
    xfail(x, 0, "unknown type '%s' in the native backend", n);
    return &ty_int;
}

static Field *field_find(Type *st, const char *name) {
    if (!st || st->kind != TY_STRUCT || !name) return NULL;
    for (int i = 0; i < st->nfields; i++)
        if (!strcmp(st->fields[i].name, name)) return &st->fields[i];
    return NULL;
}

/* Resolve a dotted name (`a.b`, `a.b.c`) into a base variable, a byte offset
   from that variable and the type stored there. The parser folds dotted
   names into a single identifier in a few positions (print arguments among
   them), so the lookup cannot rely on the symbol table alone. */
static bool resolve_dotted(const char *name, Var **base_out, int *off_out,
                           Type **ty_out, bool *array_out) {
    const char *dot = NULL;
    for (const char *p = name; p && *p; p++)
        if (*p == '.') dot = p;
    if (!dot || dot == name || !dot[1]) return false;

    char head[128];
    size_t n = (size_t)(dot - name);
    if (n >= sizeof head) return false;
    memcpy(head, name, n);
    head[n] = '\0';

    Var *base;
    int off;
    Type *t;
    bool nested_array = false;
    Var *v = find_var(head);
    if (v) {
        base = v;
        off = 0;
        t = v->ty;
        /* `p.field` on a pointer goes through the pointee */
        if (t->kind == TY_PTR && t->base) t = t->base;
    } else if (!resolve_dotted(head, &base, &off, &t, &nested_array) || nested_array) {
        /* an array field has elements, not fields of its own */
        return false;
    }

    if (!t || t->kind != TY_STRUCT) return false;
    Field *f = field_find(t, dot + 1);
    if (!f) return false;

    *base_out = base;
    *off_out = off + f->off;
    *array_out = f->count > 0;
    *ty_out = f->count > 0 ? mk_ptr(f->ty) : f->ty;
    return true;
}

/* ─────────────────────────────────────────────
   Constant folding
   ───────────────────────────────────────────── */
static bool const_eval_expr(ASTNode *n, int64_t *out);

static bool const_eval_expr(ASTNode *n, int64_t *out) {
    if (!n || !out) return false;
    switch (n->kind) {
    case NODE_LITERAL_INT:  *out = n->ival; return true;
    case NODE_LITERAL_BOOL: *out = n->bval ? 1 : 0; return true;
    case NODE_IDENT: {
        int64_t v;
        if (find_enum(n->sval, &v)) { *out = v; return true; }
        return false;
    }
    case NODE_EXPR_UNOP: {
        if (n->children.count < 1) return false;
        int64_t a;
        if (!const_eval_expr(n->children.items[0], &a)) return false;
        const char *op = n->op ? n->op : "-";
        if (!strcmp(op, "-")) { *out = -a; return true; }
        if (!strcmp(op, "~")) { *out = ~a; return true; }
        if (!strcmp(op, "!")) { *out = !a; return true; }
        return false;
    }
    case NODE_EXPR_BINOP: {
        if (n->children.count < 2) return false;
        int64_t a, b;
        if (!const_eval_expr(n->children.items[0], &a)) return false;
        if (!const_eval_expr(n->children.items[1], &b)) return false;
        const char *op = n->op ? n->op : "+";
        if (!strcmp(op, "+"))  { *out = a + b; return true; }
        if (!strcmp(op, "-"))  { *out = a - b; return true; }
        if (!strcmp(op, "*"))  { *out = a * b; return true; }
        if (!strcmp(op, "/"))  { if (!b) return false; *out = a / b; return true; }
        if (!strcmp(op, "%"))  { if (!b) return false; *out = a % b; return true; }
        if (!strcmp(op, "&"))  { *out = a & b; return true; }
        if (!strcmp(op, "|"))  { *out = a | b; return true; }
        if (!strcmp(op, "^"))  { *out = a ^ b; return true; }
        if (!strcmp(op, "<<")) { *out = a << b; return true; }
        if (!strcmp(op, ">>")) { *out = a >> b; return true; }
        if (!strcmp(op, "==")) { *out = a == b; return true; }
        if (!strcmp(op, "!=")) { *out = a != b; return true; }
        if (!strcmp(op, "<"))  { *out = a < b; return true; }
        if (!strcmp(op, "<=")) { *out = a <= b; return true; }
        if (!strcmp(op, ">"))  { *out = a > b; return true; }
        if (!strcmp(op, ">=")) { *out = a >= b; return true; }
        if (!strcmp(op, "&&")) { *out = a && b; return true; }
        if (!strcmp(op, "||")) { *out = a || b; return true; }
        return false;
    }
    case NODE_EXPR_TERNARY: {
        if (n->children.count < 3) return false;
        int64_t c;
        if (!const_eval_expr(n->children.items[0], &c)) return false;
        return const_eval_expr(n->children.items[c ? 1 : 2], out);
    }
    default: return false;
    }
}

/* ─────────────────────────────────────────────
   Expression typing
   ───────────────────────────────────────────── */
static Type *etype(X *x, ASTNode *n);

static Type *etype(X *x, ASTNode *n) {
    if (!n) return &ty_void;
    switch (n->kind) {
    case NODE_LITERAL_INT:   return &ty_int;
    case NODE_LITERAL_BOOL:  return &ty_bool;
    case NODE_LITERAL_STR:   return &ty_str;
    case NODE_LITERAL_FLOAT: return &ty_float;
    case NODE_IDENT: {
        Var *v = find_var(n->sval);
        if (v) return v->is_array ? mk_ptr(v->ty) : v->ty;
        {
            Var *b; int off; Type *t; bool arr;
            if (resolve_dotted(n->sval, &b, &off, &t, &arr)) return t;
        }
        int64_t ev;
        if (find_enum(n->sval, &ev)) return &ty_int;
        return &ty_int;
    }
    case NODE_EXPR_INDEX: {
        if (n->children.count < 1) return &ty_int;
        Type *b = etype(x, n->children.items[0]);
        if (is_ptr_like(b)) return b->base ? b->base : &ty_byte;
        return &ty_int;
    }
    case NODE_EXPR_FIELD: {
        if (n->children.count < 1) return &ty_int;
        Type *b = etype(x, n->children.items[0]);
        if (b->kind == TY_PTR) b = b->base;
        if (b && b->kind == TY_STRUCT) {
            Field *f = field_find(b, n->sval);
            if (f) return f->count > 0 ? mk_ptr(f->ty) : f->ty;
        }
        return &ty_int;
    }
    case NODE_EXPR_DEREF: {
        if (n->children.count < 1) return &ty_byte;
        Type *b = etype(x, n->children.items[0]);
        return (b->kind == TY_PTR && b->base) ? b->base : &ty_byte;
    }
    case NODE_EXPR_REF:
        if (n->children.count < 1) return mk_ptr(&ty_int);
        return mk_ptr(etype(x, n->children.items[0]));
    case NODE_EXPR_BINOP: {
        const char *op = n->op ? n->op : "+";
        if (!strcmp(op, "==") || !strcmp(op, "!=") || !strcmp(op, "<") ||
            !strcmp(op, "<=") || !strcmp(op, ">") || !strcmp(op, ">=") ||
            !strcmp(op, "&&") || !strcmp(op, "||")) return &ty_bool;
        if (n->children.count < 2) return &ty_int;
        Type *l = etype(x, n->children.items[0]);
        Type *r = etype(x, n->children.items[1]);
        if (l->kind == TY_I64 || r->kind == TY_I64) return &ty_i64;
        if (is_ptr_like(l)) return l;
        return &ty_int;
    }
    case NODE_EXPR_UNOP: {
        const char *op = n->op ? n->op : "-";
        if (!strcmp(op, "!")) return &ty_bool;
        if (!strcmp(op, "sizeof")) return &ty_i64;
        if (!strncmp(op, "(cast:", 6)) return type_from_name(x, n->type_name);
        if (n->children.count < 1) return &ty_int;
        return etype(x, n->children.items[0]);
    }
    case NODE_EXPR_TERNARY:
        if (n->children.count < 3) return &ty_int;
        return etype(x, n->children.items[1]);
    case NODE_FUNC_CALL: {
        Fn *f = find_fn(n->sval);
        return f ? f->ret : &ty_int;
    }
    case NODE_NEW_EXPR: {
        Type *t = type_from_name(x, n->type_name ? n->type_name :
                                    (n->sval ? n->sval : "int"));
        return mk_ptr(t);
    }
    case NODE_ALLOC: {
        Type *t = type_from_name(x, n->type_name ? n->type_name : "byte");
        return mk_ptr(t);
    }
    default: return &ty_int;
    }
}

/* ─────────────────────────────────────────────
   Forward declarations
   ───────────────────────────────────────────── */
static void gen_stmt(X *x, ASTNode *n);
static void gen_expr(X *x, ASTNode *n);
static void gen_addr(X *x, ASTNode *n);

static int temp_disp(X *x, int i) { return -x->frame + 8 * i; }

static void temp_store(X *x, int i) { i_mov_mem_reg(x, temp_disp(x, i), 0); }
static void temp_load_rax(X *x, int i) { i_mov_reg_mem(x, 0, temp_disp(x, i)); }
static void temp_load_rcx(X *x, int i) { i_mov_reg_mem(x, 1, temp_disp(x, i)); }

/* Emit `lea rcx, [rbp-off]` and store rax there with element type t */
static void store_local_at(X *x, int off, int delta, Type *t) {
    i_lea_mem(x, 1, off - delta);
    i_store_at_rcx(x, t);
}

/* Zero a region of the frame, byte by byte. */
static void zero_region(X *x, int off, int bytes) {
    if (bytes <= 0) return;
    int loop = new_label(x), done = new_label(x);
    i_lea_mem(x, 1, off);              /* rcx = start */
    i_mov_rax_imm(x, bytes);           /* rax = count */
    place_label(x, loop);
    e8(x, 0x48); e8(x, 0x83); e8(x, 0xF8); e8(x, 0x00);  /* cmp rax, 0 */
    i_jcc(x, CC_E, done);
    e8(x, 0xC6); e8(x, 0x01); e8(x, 0x00);               /* mov byte [rcx], 0 */
    e8(x, 0x48); e8(x, 0xFF); e8(x, 0xC1);               /* inc rcx */
    e8(x, 0x48); e8(x, 0xFF); e8(x, 0xC8);               /* dec rax */
    i_jmp(x, loop);
    place_label(x, done);
}

/* ─────────────────────────────────────────────
   Address generation
   ───────────────────────────────────────────── */
static void gen_addr_base(X *x, ASTNode *n) {
    Type *t = etype(x, n);
    if (is_ptr_like(t)) gen_expr(x, n);
    else gen_addr(x, n);
}

static void gen_addr(X *x, ASTNode *n) {
    if (x->failed || !n) return;
    switch (n->kind) {
    case NODE_IDENT: {
        Var *v = find_var(n->sval);
        if (!v) {
            Var *b; int off; Type *t; bool arr;
            if (!resolve_dotted(n->sval, &b, &off, &t, &arr)) {
                xfail(x, n->line, "unknown variable '%s'", n->sval ? n->sval : "?");
                return;
            }
            if (b->global) i_lea_data(x, b->off);
            else i_lea_mem(x, 0, b->off);
            if (off) { e8(x, 0x48); e8(x, 0x05); e32(x, off); }  /* add rax, off */
            return;
        }
        if (v->global) i_lea_data(x, v->off);
        else i_lea_mem(x, 0, v->off);
        return;
    }
    case NODE_EXPR_INDEX: {
        if (n->children.count < 2) { xfail(x, n->line, "malformed index expression"); return; }
        ASTNode *base = n->children.items[0];
        Type *bt = etype(x, base);
        Type *elem = is_ptr_like(bt) ? (bt->base ? bt->base : &ty_byte) : &ty_int;
        int esz = elem->size > 0 ? elem->size : 1;

        int t = x->temp_depth++;
        gen_addr_base(x, base);
        if (x->failed) { x->temp_depth = t; return; }
        temp_store(x, t);

        gen_expr(x, n->children.items[1]);
        if (x->failed) { x->temp_depth = t; return; }
        i_mov_reg_reg(x, 1, 0);           /* rcx = index */
        i_mov_rax_imm(x, esz);            /* rax = elem size */
        i_imul(x);                        /* rax = index * size */
        temp_load_rcx(x, t);              /* rcx = base */
        e8(x, 0x48); e8(x, 0x01); e8(x, 0xC1); /* add rcx, rax */
        i_mov_reg_reg(x, 0, 1);           /* rax = rcx */
        x->temp_depth = t;
        return;
    }
    case NODE_EXPR_FIELD: {
        if (n->children.count < 1) { xfail(x, n->line, "malformed field access"); return; }
        ASTNode *base = n->children.items[0];
        Type *bt = etype(x, base);
        bool arrow = n->op && !strcmp(n->op, "->");
        if (bt->kind == TY_PTR) arrow = true;
        if (bt->kind == TY_PTR) bt = bt->base;

        if (arrow) gen_expr(x, base);
        else gen_addr(x, base);
        if (x->failed) return;

        if (!bt || bt->kind != TY_STRUCT) {
            xfail(x, n->line, "field access '.'%s on a non-struct type",
                  n->sval ? n->sval : "");
            return;
        }
        Field *f = field_find(bt, n->sval);
        if (!f) { xfail(x, n->line, "struct '%s' has no field '%s'", bt->name, n->sval ? n->sval : "?"); return; }
        if (f->off) {
            e8(x, 0x48); e8(x, 0x05); e32(x, f->off);   /* add rax, imm32 */
        }
        return;
    }
    case NODE_EXPR_DEREF:
        if (n->children.count < 1) { xfail(x, n->line, "malformed dereference"); return; }
        gen_expr(x, n->children.items[0]);
        return;
    default:
        xfail(x, n->line, "expression is not assignable in the native backend");
        return;
    }
}

/* ─────────────────────────────────────────────
   Print helpers
   ───────────────────────────────────────────── */
static int g_true_off = -1, g_false_off = -1, g_nl_off = -1;

static void call_fn_named(X *x, const char *name, int line) {
    Fn *f = find_fn(name);
    if (!f) { xfail(x, line, "runtime helper '%s' is missing", name); return; }
    i_call(x, f->label);
}

static void print_value(X *x, ASTNode *n) {
    Type *t = etype(x, n);
    gen_expr(x, n);
    if (x->failed) return;
    if (t->kind == TY_BOOL) {
        int lf = new_label(x), le = new_label(x);
        i_test_rax(x);
        i_jcc(x, CC_E, lf);
        i_lea_data(x, g_true_off);
        i_mov_reg_reg(x, 7, 0);
        call_fn_named(x, "lu_puts", n->line);
        i_jmp(x, le);
        place_label(x, lf);
        i_lea_data(x, g_false_off);
        i_mov_reg_reg(x, 7, 0);
        call_fn_named(x, "lu_puts", n->line);
        place_label(x, le);
        return;
    }
    if (t->kind == TY_STR ||
        (t->kind == TY_PTR && t->base &&
         (t->base->kind == TY_BYTE || t->base->kind == TY_BOOL))) {
        /* `str` and byte buffers are NUL-terminated text by convention */
        i_mov_reg_reg(x, 7, 0);
        call_fn_named(x, "lu_puts", n->line);
        return;
    }
    if (t->kind == TY_PTR) {
        /* other pointers print as their numeric address, which is what you
           want when you are poking at memory */
        i_mov_reg_reg(x, 7, 0);
        call_fn_named(x, "lu_putn", n->line);
        return;
    }
    if (t->kind == TY_STRUCT || t->kind == TY_VOID) {
        xfail(x, n->line, "cannot print a value of this type natively");
        return;
    }
    i_mov_reg_reg(x, 7, 0);
    call_fn_named(x, "lu_putn", n->line);
}

static void emit_newline(X *x) {
    i_lea_data(x, g_nl_off);
    i_mov_reg_reg(x, 7, 0);
    call_fn_named(x, "lu_puts", 0);
}

/* ─────────────────────────────────────────────
   Binary operators
   ───────────────────────────────────────────── */
static bool is_cmp_op(const char *op) {
    return !strcmp(op, "==") || !strcmp(op, "!=") || !strcmp(op, "<") ||
           !strcmp(op, "<=") || !strcmp(op, ">") || !strcmp(op, ">=");
}

static void gen_binop(X *x, ASTNode *n) {
    if (n->children.count < 2) { xfail(x, n->line, "malformed binary expression"); return; }
    const char *op = n->op ? n->op : "+";
    ASTNode *lhs = n->children.items[0];
    ASTNode *rhs = n->children.items[1];

    /* short-circuit logic */
    if (!strcmp(op, "&&") || !strcmp(op, "||")) {
        int lshort = new_label(x), lend = new_label(x);
        gen_expr(x, lhs);
        if (x->failed) return;
        i_test_rax(x);
        if (!strcmp(op, "&&")) i_jcc(x, CC_E, lshort);
        else                   i_jcc(x, CC_NE, lshort);
        gen_expr(x, rhs);
        if (x->failed) return;
        i_test_rax(x);
        i_jcc(x, CC_E, lshort);
        i_mov_rax_imm(x, 1);
        i_jmp(x, lend);
        place_label(x, lshort);
        i_mov_rax_imm(x, 0);
        place_label(x, lend);
        return;
    }

    Type *lt = etype(x, lhs);
    bool ptr_arith = is_ptr_like(lt) &&
                     (!strcmp(op, "+") || !strcmp(op, "-"));

    int t = x->temp_depth++;
    gen_expr(x, lhs);
    if (x->failed) { x->temp_depth = t; return; }
    temp_store(x, t);
    gen_expr(x, rhs);
    if (x->failed) { x->temp_depth = t; return; }

    if (ptr_arith) {
        /* scale the integer operand by the pointee size */
        int esz = (lt->base && lt->base->size > 0) ? lt->base->size : 1;
        if (esz != 1) {
            i_mov_reg_reg(x, 1, 0);
            i_mov_rax_imm(x, esz);
            i_imul(x);
        }
    }

    i_mov_reg_reg(x, 1, 0);     /* rcx = rhs */
    temp_load_rax(x, t);        /* rax = lhs */
    x->temp_depth = t;

    if (!strcmp(op, "+"))  { i_add(x); return; }
    if (!strcmp(op, "-"))  { i_sub(x); return; }
    if (!strcmp(op, "*"))  { i_imul(x); return; }
    if (!strcmp(op, "%"))  { i_idiv(x); i_mod_rax(x); return; }
    if (!strcmp(op, "/"))  { i_idiv(x); return; }
    if (!strcmp(op, "&"))  { i_and(x); return; }
    if (!strcmp(op, "|"))  { i_or(x); return; }
    if (!strcmp(op, "^"))  { i_xor(x); return; }
    if (!strcmp(op, "<<")) { i_shl(x); return; }
    if (!strcmp(op, ">>")) {
        if (lt->kind == TY_I64 || lt->kind == TY_INT) i_sar(x); else i_shr(x);
        return;
    }
    if (is_cmp_op(op)) {
        i_cmp_rax_rcx(x);
        if (!strcmp(op, "==")) i_setcc(x, CC_E);
        else if (!strcmp(op, "!=")) i_setcc(x, CC_NE);
        else if (!strcmp(op, "<"))  i_setcc(x, CC_L);
        else if (!strcmp(op, "<=")) i_setcc(x, CC_LE);
        else if (!strcmp(op, ">"))  i_setcc(x, CC_G);
        else                        i_setcc(x, CC_GE);
        return;
    }
    xfail(x, n->line, "operator '%s' is not supported by the native backend", op);
}

/* ─────────────────────────────────────────────
   Calls
   ───────────────────────────────────────────── */
static const int SYSCALL_REGS[6] = { 7, 6, 2, 10, 8, 9 };  /* rdi rsi rdx r10 r8 r9 */
static const int CALL_REGS[6]    = { 7, 6, 2, 1, 8, 9 };   /* System V            */

static void eval_args(X *x, ASTNode *n, const int *regs) {
    int base = x->temp_depth;
    int cnt = n->children.count;
    if (cnt > 6) {
        xfail(x, n->line, "native calls support at most 6 arguments (got %d)", cnt);
        return;
    }
    for (int i = 0; i < cnt; i++) {
        x->temp_depth = base + i + 1;
        gen_expr(x, n->children.items[i]);
        if (x->failed) { x->temp_depth = base; return; }
        temp_store(x, base + i);
    }
    for (int i = 0; i < cnt; i++) {
        temp_load_rax(x, base + i);
        i_mov_reg_reg(x, regs[i], 0);
    }
    x->temp_depth = base;
}

static void gen_call(X *x, ASTNode *n) {
    const char *name = n->sval ? n->sval : "";

    if (!strcmp(name, "syscall") || !strcmp(name, "sys")) {
        if (n->children.count < 1) { xfail(x, n->line, "syscall() needs a syscall number"); return; }
        int base = x->temp_depth;
        int cnt = n->children.count;
        if (cnt > 7) { xfail(x, n->line, "syscall() takes at most 7 operands"); return; }
        for (int i = 0; i < cnt; i++) {
            x->temp_depth = base + i + 1;
            gen_expr(x, n->children.items[i]);
            if (x->failed) { x->temp_depth = base; return; }
            temp_store(x, base + i);
        }
        for (int i = 1; i < cnt; i++) {
            temp_load_rax(x, base + i);
            i_mov_reg_reg(x, SYSCALL_REGS[i - 1], 0);
        }
        temp_load_rax(x, base + 0);   /* syscall number */
        i_syscall(x);
        x->temp_depth = base;
        return;
    }

    if (!strcmp(name, "exit")) {
        if (n->children.count >= 1) {
            gen_expr(x, n->children.items[0]);
            i_mov_reg_reg(x, 7, 0);
        } else {
            i_mov_reg_imm(x, 7, 0);
        }
        i_mov_reg_imm(x, 0, 60);
        i_syscall(x);
        return;
    }

    /* strlen has no libc to lean on here; the prelude's own loop does it. */
    if (!strcmp(name, "strlen")) {
        if (n->children.count != 1) {
            xfail(x, n->line, "strlen() takes exactly 1 argument");
            return;
        }
        eval_args(x, n, CALL_REGS);
        if (x->failed) return;
        call_fn_named(x, "lu_strlen", n->line);
        return;
    }

    Fn *f = find_fn(name);
    if (!f) {
        xfail(x, n->line, "unknown function '%s' in the native backend", name);
        return;
    }
    if (n->children.count != f->nparams)
        xfail(x, n->line, "function '%s' expects %d argument(s), got %d",
              name, f->nparams, n->children.count);
    eval_args(x, n, CALL_REGS);
    if (x->failed) return;
    i_call(x, f->label);
}

/* ─────────────────────────────────────────────
   Expressions
   ───────────────────────────────────────────── */
static void gen_expr(X *x, ASTNode *n) {
    if (x->failed || !n) return;

    switch (n->kind) {
    case NODE_LITERAL_INT:
        i_mov_rax_imm(x, n->ival);
        return;

    case NODE_LITERAL_BOOL:
        i_mov_rax_imm(x, n->bval ? 1 : 0);
        return;

    case NODE_LITERAL_FLOAT:
        xfail(x, n->line, "floating point is not supported by the native backend; use `luc -c`");
        return;

    case NODE_LITERAL_STR: {
        const char *s = n->sval ? n->sval : "";
        if (s[0] == '$' && s[1] == 'f') s += 2;
        int off = data_str(x, s);
        i_lea_data(x, off);
        return;
    }

    case NODE_IDENT: {
        Var *v = find_var(n->sval);
        if (!v) {
            int64_t ev;
            if (find_enum(n->sval, &ev)) { i_mov_rax_imm(x, ev); return; }
            Var *b; int off; Type *t; bool arr;
            if (resolve_dotted(n->sval, &b, &off, &t, &arr)) {
                gen_addr(x, n);              /* lea base + field offset */
                if (x->failed || arr) return;
                i_load_at_rax(x, t);
                return;
            }
            xfail(x, n->line, "unknown variable '%s'", n->sval ? n->sval : "?");
            return;
        }
        if (v->is_array) {
            if (v->global) i_lea_data(x, v->off);
            else i_lea_mem(x, 0, v->off);
            return;
        }
        if (v->global) {
            i_lea_data(x, v->off);
            i_load_at_rax(x, v->ty);
            return;
        }
        i_load_slot(x, v->off, v->ty);
        return;
    }

    case NODE_EXPR_BINOP:
        gen_binop(x, n);
        return;

    case NODE_EXPR_UNOP: {
        const char *op = n->op ? n->op : "-";
        if (n->children.count < 1) { xfail(x, n->line, "malformed unary expression"); return; }

        if (!strcmp(op, "sizeof")) {
            Type *t = n->type_name ? type_from_name(x, n->type_name)
                                   : etype(x, n->children.items[0]);
            i_mov_rax_imm(x, t->size);
            return;
        }
        if (!strncmp(op, "(cast:", 6)) {
            Type *t = type_from_name(x, n->type_name);
            gen_expr(x, n->children.items[0]);
            if (x->failed) return;
            if (t->kind == TY_BOOL || t->kind == TY_BYTE) { i_movzx_rax_al(x); return; }
            if (t->kind == TY_INT) { i_movsxd_rax_eax(x); return; }
            return;
        }
        if (!strcmp(op, "++") || !strcmp(op, "--")) {
            gen_addr(x, n->children.items[0]);
            if (x->failed) return;
            int t = x->temp_depth++;
            temp_store(x, t);
            i_load_at_rax(x, etype(x, n->children.items[0]));
            i_mov_reg_reg(x, 1, 0);
            i_mov_rax_imm(x, 1);
            if (!strcmp(op, "++")) i_add(x); else i_sub(x);
            i_mov_reg_reg(x, 2, 0);           /* rdx = new value */
            temp_load_rcx(x, t);              /* rcx = address */
            i_mov_reg_reg(x, 0, 2);
            i_store_at_rcx(x, etype(x, n->children.items[0]));
            i_mov_reg_reg(x, 0, 2);
            x->temp_depth = t;
            return;
        }

        gen_expr(x, n->children.items[0]);
        if (x->failed) return;
        if (!strcmp(op, "-")) { i_neg(x); return; }
        if (!strcmp(op, "~")) { i_not(x); return; }
        if (!strcmp(op, "!")) {
            i_test_rax(x);
            i_setcc(x, CC_E);
            return;
        }
        if (!strcmp(op, "+")) return;
        xfail(x, n->line, "unary operator '%s' is not supported natively", op);
        return;
    }

    case NODE_EXPR_TERNARY: {
        if (n->children.count < 3) { xfail(x, n->line, "malformed conditional expression"); return; }
        int lelse = new_label(x), lend = new_label(x);
        gen_expr(x, n->children.items[0]);
        if (x->failed) return;
        i_test_rax(x);
        i_jcc(x, CC_E, lelse);
        gen_expr(x, n->children.items[1]);
        i_jmp(x, lend);
        place_label(x, lelse);
        gen_expr(x, n->children.items[2]);
        place_label(x, lend);
        return;
    }

    case NODE_EXPR_INDEX: {
        if (n->op && !strcmp(n->op, "slice")) {
            xfail(x, n->line, "slices are not supported by the native backend");
            return;
        }
        gen_addr(x, n);
        if (x->failed) return;
        i_load_at_rax(x, etype(x, n));
        return;
    }

    case NODE_EXPR_FIELD: {
        if (n->children.count < 1) { xfail(x, n->line, "malformed field access"); return; }
        Type *ft = etype(x, n);
        gen_addr(x, n);
        if (x->failed) return;
        if (ft->kind == TY_STRUCT) return;      /* struct value: address only */
        i_load_at_rax(x, ft);
        return;
    }

    case NODE_EXPR_DEREF:
        if (n->children.count < 1) { xfail(x, n->line, "malformed dereference"); return; }
        gen_expr(x, n->children.items[0]);
        if (x->failed) return;
        i_load_at_rax(x, etype(x, n));
        return;

    case NODE_EXPR_REF:
        if (n->children.count < 1) { xfail(x, n->line, "malformed address-of"); return; }
        gen_addr(x, n->children.items[0]);
        return;

    case NODE_FUNC_CALL:
        gen_call(x, n);
        return;

    case NODE_ALLOC: {
        /* Alloc/T:n → n * sizeof(T) bytes from the runtime allocator */
        Type *t = type_from_name(x, n->type_name ? n->type_name : "byte");
        if (n->children.count < 1) { xfail(x, n->line, "Alloc/ needs a count"); return; }
        gen_expr(x, n->children.items[0]);
        if (x->failed) return;
        if (t->size != 1) {
            i_mov_reg_reg(x, 1, 0);
            i_mov_rax_imm(x, t->size);
            i_imul(x);
        }
        i_mov_reg_reg(x, 7, 0);
        call_fn_named(x, "lu_alloc", n->line);
        return;
    }

    case NODE_NEW_EXPR: {
        Type *t = type_from_name(x, n->type_name ? n->type_name
                                                 : (n->sval ? n->sval : "int"));
        i_mov_reg_imm(x, 7, t->size > 0 ? t->size : 8);
        call_fn_named(x, "lu_alloc", n->line);
        i_mov_reg_reg(x, 2, 0);                 /* rdx = ptr */
        i_mov_reg_reg(x, 7, 0);
        i_mov_reg_imm(x, 6, t->size > 0 ? t->size : 8);
        i_mov_reg_imm(x, 2, 0);
        call_fn_named(x, "lu_memset", n->line);  /* rdi=ptr rsi=0 rdx=size */
        i_mov_reg_reg(x, 0, 2);
        return;
    }

    case NODE_FSTRING: {
        /* Only fully literal f-strings can be lowered without a formatting
           runtime. Everything else keeps using the C backend. */
        int ok = 1;
        for (int i = 0; i < n->children.count; i++) {
            ASTNode *p = n->children.items[i];
            if (!p || p->kind != NODE_LITERAL_STR) { ok = 0; break; }
        }
        if (!ok) {
            xfail(x, n->line, "f-strings with interpolated values need `luc -c`");
            return;
        }
        char buf[1024];
        buf[0] = '\0';
        for (int i = 0; i < n->children.count; i++)
            strncat(buf, n->children.items[i]->sval ? n->children.items[i]->sval : "",
                    sizeof(buf) - strlen(buf) - 1);
        int off = data_str(x, buf);
        i_lea_data(x, off);
        return;
    }

    default:
        xfail(x, n->line, "expression kind %d is not supported by the native backend", (int)n->kind);
        return;
    }
}

/* ─────────────────────────────────────────────
   Local declaration sizing
   ───────────────────────────────────────────── */
static int decl_bytes(X *x, ASTNode *n) {
    if (n->kind == NODE_PTR_DECL) return 8;
    Type *t = type_from_name(x, n->type_name);
    if (n->ival > 0) {
        int64_t count = 1;
        if (n->children.count > 0 && !const_eval_expr(n->children.items[0], &count))
            count = 1;
        if (count < 1) count = 1;
        if (count > 1 << 20) count = 1 << 20;
        int64_t bytes = count * (int64_t)t->size;
        if (bytes > (1 << 24)) bytes = 1 << 24;
        return align8((int)bytes);
    }
    int sz = t->size < 8 ? 8 : t->size;
    return align8(sz);
}

static int count_locals(X *x, ASTNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case NODE_VAR_DECL:
    case NODE_PTR_DECL:
        return decl_bytes(x, n);
    case NODE_FUNC_DECL:
    case NODE_ASYNC_FUNC:
        return 0;
    case NODE_LOOP:
        return 8 + count_locals(x, n->children.count > 1 ? n->children.items[1] : NULL);
    case NODE_LOOP_EACH:
        return 16 + count_locals(x, n->children.count > 2 ? n->children.items[2] : NULL);
    default:
        break;
    }
    int s = 0;
    for (int i = 0; i < n->children.count; i++)
        s += count_locals(x, n->children.items[i]);
    return s;
}

/* ─────────────────────────────────────────────
   Statements
   ───────────────────────────────────────────── */
static void gen_var_decl(X *x, ASTNode *n) {
    Type *t;
    if (n->kind == NODE_PTR_DECL) {
        t = mk_ptr(type_from_name(x, n->type_name ? n->type_name : "byte"));
    } else if (n->type_name && !strcmp(n->type_name, "auto")) {
        if (n->children.count < 1) {
            xfail(x, n->line, "'var %s' needs an initialiser so its type can be inferred",
                  n->sval ? n->sval : "?");
            return;
        }
        t = etype(x, n->children.items[0]);
    } else {
        t = type_from_name(x, n->type_name);
    }
    if (x->failed) return;

    if (n->ival > 1) {
        xfail(x, n->line, "multi-dimensional arrays need `luc -c`");
        return;
    }

    if (n->ival == 1) {
        int64_t count = 0;
        if (n->children.count < 1 || !const_eval_expr(n->children.items[0], &count)) {
            xfail(x, n->line, "array size for '%s' must be a compile-time constant",
                  n->sval ? n->sval : "?");
            return;
        }
        if (count < 0) count = 0;
        if (count > (1 << 20)) { xfail(x, n->line, "array '%s' is too large", n->sval); return; }
        int bytes = align8((int)(count * t->size));
        x->locals_used = align8(x->locals_used);
        int off = x->locals_used + bytes;
        if (off > 1 << 24) { xfail(x, n->line, "stack frame too large"); return; }
        x->locals_used = off;

        push_var(n->sval, t, true, (int)count, off, false);
        zero_region(x, off, bytes);

        int ninit = n->children.count - 1;
        for (int i = 0; i < ninit && i < count; i++) {
            gen_expr(x, n->children.items[1 + i]);
            if (x->failed) return;
            store_local_at(x, off, (int)(i * t->size), t);
        }
        return;
    }

    int bytes = decl_bytes(x, n);
    x->locals_used = align8(x->locals_used);
    int off = x->locals_used + bytes;
    x->locals_used = off;

    push_var(n->sval, t, false, 0, off, false);

    if (t->kind == TY_STRUCT) zero_region(x, off, t->size);

    if (n->children.count > 0) {
        gen_expr(x, n->children.items[0]);
        if (x->failed) return;
        i_store_slot(x, off, t);
    }
}

static void gen_set(X *x, ASTNode *n) {
    if (!n->lhs) {
        xfail(x, n->line, "assignment target could not be resolved natively "
                          "(use `luc -c` if this is a class field)");
        return;
    }
    if (n->children.count < 1) { xfail(x, n->line, "assignment without a value"); return; }

    Type *t = etype(x, n->lhs);
    int t0 = x->temp_depth++;

    gen_addr(x, n->lhs);
    if (x->failed) { x->temp_depth = t0; return; }
    temp_store(x, t0);

    const char *op = n->op;
    int t1 = -1;
    if (op && *op) {
        t1 = x->temp_depth++;
        temp_load_rax(x, t0);
        i_load_at_rax(x, t);
        temp_store(x, t1);
    }

    gen_expr(x, n->children.items[0]);
    if (x->failed) { x->temp_depth = t0; return; }
    int t2 = x->temp_depth++;
    temp_store(x, t2);

    if (op && *op) {
        temp_load_rax(x, t1);
        temp_load_rcx(x, t2);
        if (!strcmp(op, "+")) i_add(x);
        else if (!strcmp(op, "-")) i_sub(x);
        else if (!strcmp(op, "*")) i_imul(x);
        else if (!strcmp(op, "/")) i_idiv(x);
        else if (!strcmp(op, "%")) { i_idiv(x); i_mod_rax(x); }
        else if (!strcmp(op, "&")) i_and(x);
        else if (!strcmp(op, "|")) i_or(x);
        else if (!strcmp(op, "^")) i_xor(x);
        else if (!strcmp(op, "<<")) i_shl(x);
        else if (!strcmp(op, ">>")) i_sar(x);
        else xfail(x, n->line, "compound operator '%s' is not supported natively", op);
        x->temp_depth = t0;
    } else {
        temp_load_rax(x, t2);
        x->temp_depth = t0;
    }

    temp_load_rcx(x, t0);
    i_store_at_rcx(x, t);
}

static void gen_if(X *x, ASTNode *n) {
    if (n->children.count < 2) { xfail(x, n->line, "malformed if statement"); return; }
    int lelse = new_label(x), lend = new_label(x);
    gen_expr(x, n->children.items[0]);
    if (x->failed) return;
    i_test_rax(x);
    i_jcc(x, CC_E, lelse);
    gen_stmt(x, n->children.items[1]);
    i_jmp(x, lend);
    place_label(x, lelse);
    if (n->children.count > 2 && n->children.items[2])
        gen_stmt(x, n->children.items[2]);
    place_label(x, lend);
}

static void gen_loop_while(X *x, ASTNode *n) {
    if (n->children.count < 2) { xfail(x, n->line, "malformed while loop"); return; }
    int top = new_label(x), end = new_label(x);
    place_label(x, top);
    gen_expr(x, n->children.items[0]);
    if (x->failed) return;
    i_test_rax(x);
    i_jcc(x, CC_E, end);
    if (x->nctrl < 64) { x->brk[x->nctrl] = end; x->cont[x->nctrl] = top; x->nctrl++; }
    gen_stmt(x, n->children.items[1]);
    if (x->nctrl > 0) x->nctrl--;
    i_jmp(x, top);
    place_label(x, end);
}

static void gen_loop_count(X *x, ASTNode *n) {
    if (n->children.count < 2) { xfail(x, n->line, "malformed loop"); return; }
    /* hidden counter slot */
    x->locals_used = align8(x->locals_used);
    int off = x->locals_used + 8;
    x->locals_used = off;

    gen_expr(x, n->children.items[0]);
    if (x->failed) return;
    i_store_slot(x, off, &ty_i64);

    /* `continue` must still decrement the counter, so the decrement lives at
       its own label rather than at the end of the body. */
    int top = new_label(x), cont = new_label(x), end = new_label(x);
    place_label(x, top);
    i_load_slot(x, off, &ty_i64);
    i_test_rax(x);
    i_jcc(x, CC_LE, end);
    if (x->nctrl < 64) { x->brk[x->nctrl] = end; x->cont[x->nctrl] = cont; x->nctrl++; }
    gen_stmt(x, n->children.items[1]);
    if (x->nctrl > 0) x->nctrl--;
    place_label(x, cont);
    i_load_slot(x, off, &ty_i64);
    i_dec_rax(x);                        /* counter = counter - 1 */
    i_store_slot(x, off, &ty_i64);
    i_jmp(x, top);
    place_label(x, end);
}

static void gen_loop_each(X *x, ASTNode *n) {
    if (n->children.count < 3) { xfail(x, n->line, "malformed for-each loop"); return; }
    ASTNode *item = n->children.items[0];
    ASTNode *list = n->children.items[1];
    Var *av = list && list->kind == NODE_IDENT ? find_var(list->sval) : NULL;
    if (!av || !av->is_array) {
        xfail(x, n->line, "for-each over '%s' needs a local array in the native backend",
              (list && list->sval) ? list->sval : "?");
        return;
    }
    Type *elem = av->ty;
    int esz = elem->size > 0 ? elem->size : 1;

    x->locals_used = align8(x->locals_used);
    int off_idx  = x->locals_used + 8;  x->locals_used = off_idx;
    int off_item = x->locals_used + 8;  x->locals_used = off_item;

    push_var(item->sval, elem, false, 0, off_item, false);

    if (av->global) i_lea_data(x, av->off);
    else i_lea_mem(x, 0, av->off);
    i_store_slot(x, off_idx, &ty_i64);   /* reuse: store base pointer in idx slot */

    /* second hidden slot for the counter */
    int off_cnt = x->locals_used + 8;
    x->locals_used = off_cnt;
    i_mov_rax_imm(x, 0);
    i_store_slot(x, off_cnt, &ty_i64);

    int top = new_label(x), cont = new_label(x), end = new_label(x);
    place_label(x, top);
    i_load_slot(x, off_cnt, &ty_i64);
    i_mov_reg_reg(x, 1, 0);
    i_mov_rax_imm(x, av->count);
    i_cmp_rax_rcx(x);                    /* count vs counter */
    i_jcc(x, CC_LE, end);                /* stop once count <= counter */

    /* rax = base + i*elem_size */
    i_load_slot(x, off_idx, &ty_i64);
    i_mov_reg_reg(x, 2, 0);              /* rdx = base */
    i_load_slot(x, off_cnt, &ty_i64);
    if (esz != 1) {
        i_mov_reg_reg(x, 1, 0);
        i_mov_rax_imm(x, esz);
        i_imul(x);
    }
    e8(x, 0x48); e8(x, 0x01); e8(x, 0xD0);  /* add rax, rdx */
    i_load_at_rax(x, elem);
    i_store_slot(x, off_item, elem);

    if (x->nctrl < 64) { x->brk[x->nctrl] = end; x->cont[x->nctrl] = cont; x->nctrl++; }
    gen_stmt(x, n->children.items[2]);
    if (x->nctrl > 0) x->nctrl--;
    /* continue lands here so the counter still advances */
    place_label(x, cont);
    i_load_slot(x, off_cnt, &ty_i64);
    i_mov_reg_reg(x, 1, 0);
    i_mov_rax_imm(x, 1);
    i_add(x);
    i_store_slot(x, off_cnt, &ty_i64);
    i_jmp(x, top);
    place_label(x, end);
}

static void gen_print(X *x, ASTNode *n) {
    if (n->children.count == 0) { emit_newline(x); return; }
    for (int i = 0; i < n->children.count; i++) {
        if (i) {
            /* one space between values */
            i_mov_rax_imm(x, 32);
            i_mov_reg_reg(x, 7, 0);
            call_fn_named(x, "lu_putc", n->line);
        }
        print_value(x, n->children.items[i]);
    }
    emit_newline(x);
}

static void gen_stmt(X *x, ASTNode *n) {
    if (x->failed || !n) return;

    switch (n->kind) {
    case NODE_PROGRAM:
    case NODE_NAMESPACE:
    case NODE_CLASS_BODY:
        scope_push();
        for (int i = 0; i < n->children.count; i++)
            gen_stmt(x, n->children.items[i]);
        scope_pop();
        return;

    case NODE_LANG_DECL:
    case NODE_IMPORT:
    case NODE_INCLUDE:
    case NODE_MODE:
    case NODE_DEF_CONFIG:
    case NODE_OPT:
    case NODE_ANNOT:
    case NODE_DEFINE:
    case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL:
    case NODE_UNION_DECL:
    case NODE_FUNC_DECL:
    case NODE_ASYNC_FUNC:
    case NODE_DEF_CONST:
        return;

    case NODE_VAR_DECL:
    case NODE_PTR_DECL:
        gen_var_decl(x, n);
        return;

    case NODE_SET:
        gen_set(x, n);
        return;

    case NODE_IF:
        gen_if(x, n);
        return;

    case NODE_LOOP:
        gen_loop_count(x, n);
        return;

    case NODE_LOOP_WHILE:
        gen_loop_while(x, n);
        return;

    case NODE_LOOP_EACH:
        gen_loop_each(x, n);
        return;

    case NODE_PR:
        gen_print(x, n);
        return;

    case NODE_TO:
        if (n->children.count > 0) gen_stmt(x, n->children.items[0]);
        return;

    case NODE_RETURN: {
        if (n->sval && !strcmp(n->sval, "__break__")) {
            if (x->nctrl > 0) i_jmp(x, x->brk[x->nctrl - 1]);
            else xfail(x, n->line, "break outside of a loop");
            return;
        }
        if (n->sval && !strcmp(n->sval, "__continue__")) {
            if (x->nctrl > 0) i_jmp(x, x->cont[x->nctrl - 1]);
            else xfail(x, n->line, "continue outside of a loop");
            return;
        }
        if (n->children.count > 0) {
            gen_expr(x, n->children.items[0]);
            if (x->failed) return;
        } else {
            i_mov_rax_imm(x, 0);
        }
        i_jmp(x, x->epilogue);
        return;
    }

    case NODE_FUNC_CALL:
        gen_expr(x, n);
        return;

    case NODE_ALLOC:
        gen_expr(x, n);
        return;

    case NODE_FREE:
        /* The native runtime allocates from a bump arena (mmap); there is
           nothing to release. Free/ is accepted and ignored. */
        return;

    case NODE_MEMSET: {
        if (n->children.count < 3) { xfail(x, n->line, "MemSet/ needs 3 operands"); return; }
        eval_args(x, n, CALL_REGS);
        if (x->failed) return;
        call_fn_named(x, "lu_memset", n->line);
        return;
    }

    case NODE_MEMCPY: {
        if (n->children.count < 3) { xfail(x, n->line, "MemCpy/ needs 3 operands"); return; }
        eval_args(x, n, CALL_REGS);
        if (x->failed) return;
        call_fn_named(x, "lu_memcpy", n->line);
        return;
    }

    case NODE_ASM_STMT: {
        if (n->op && !strcmp(n->op, "raw")) {
            for (int i = 0; i < n->children.count; i++) {
                ASTNode *b = n->children.items[i];
                if (b->kind != NODE_LITERAL_INT) {
                    xfail(x, n->line, "raw { } accepts integer byte values only");
                    return;
                }
                e8(x, (int)(b->ival & 0xFF));
            }
            return;
        }
        xfail(x, n->line, "asm { } takes assembler text, which the native backend "
                          "cannot parse; use `luc -c`, or `raw { 0x.. }` for bytes");
        return;
    }

    case NODE_BLOCK:
        for (int i = 0; i < n->children.count; i++)
            gen_stmt(x, n->children.items[i]);
        return;

    default:
        xfail(x, n->line, "statement kind %d is not supported by the native backend",
              (int)n->kind);
        return;
    }
}

/* ─────────────────────────────────────────────
   Function emission
   ───────────────────────────────────────────── */
static Type *fn_param_type(X *x, ASTNode *p) {
    if (p->kind == NODE_PTR_DECL)
        return mk_ptr(type_from_name(x, p->type_name ? p->type_name : "byte"));
    return type_from_name(x, p->type_name);
}

static void gen_function(X *x, ASTNode *fn) {
    Fn *f = find_fn(fn->sval);
    if (!f) return;

    ASTNode *body = NULL;
    int np = 0;
    for (int i = 0; i < fn->children.count; i++) {
        ASTNode *c = fn->children.items[i];
        if (c->kind == NODE_PROGRAM) { body = c; break; }
        np++;
    }
    if (!body) {
        xfail(x, fn->line, "function '%s' has no body; the native backend "
                           "cannot call external code", f->name);
        return;
    }
    if (np > 6) {
        xfail(x, fn->line, "the native backend supports at most 6 parameters per function");
        return;
    }

    /* saved context */
    int save_frame = x->frame;
    int save_locals = x->locals_used;
    int save_temps = x->temp_depth;
    int save_ctrl = x->nctrl;
    int save_epi = x->epilogue;
    Type *save_ret = f->ret;
    int save_nlocals = g_nlocals;
    int save_nscope = g_nscope;

    int locals = 0;
    for (int i = 0; i < np; i++) {
        Type *pt = fn_param_type(x, fn->children.items[i]);
        locals += align8(pt->size < 8 ? 8 : pt->size);
    }
    locals += count_locals(x, body);

    int frame = align16(locals + 8 * XV_MAX_TEMP);
    x->frame = frame;
    x->locals_used = 0;
    x->temp_depth = 0;
    x->nctrl = 0;
    x->epilogue = new_label(x);
    g_nlocals = 0;
    g_nscope = 0;
    scope_push();

    place_label(x, f->label);

    /* prologue */
    e8(x, 0x55);                                  /* push rbp */
    e8(x, 0x48); e8(x, 0x89); e8(x, 0xE5);        /* mov rbp, rsp */
    e8(x, 0x48); e8(x, 0x81); e8(x, 0xEC);        /* sub rsp, frame */
    e32(x, frame);

    /* parameters */
    for (int i = 0; i < np; i++) {
        ASTNode *p = fn->children.items[i];
        Type *pt = fn_param_type(x, p);
        int bytes = align8(pt->size < 8 ? 8 : pt->size);
        x->locals_used = align8(x->locals_used);
        int off = x->locals_used + bytes;
        x->locals_used = off;
        push_var(p->sval, pt, false, 0, off, false);
        int argreg = CALL_REGS[i];
        i_store_slot_reg(x, off, pt, argreg);
    }

    gen_stmt(x, body);

    i_mov_rax_imm(x, 0);
    place_label(x, x->epilogue);
    e8(x, 0xC9);                                   /* leave */
    e8(x, 0xC3);                                   /* ret */

    g_nscope = save_nscope;
    g_nlocals = save_nlocals;
    x->frame = save_frame;
    x->locals_used = save_locals;
    x->temp_depth = save_temps;
    x->nctrl = save_ctrl;
    x->epilogue = save_epi;
    f->ret = save_ret;
}

/* Emit a synthetic function from a list of statements. */
static void gen_synthetic(X *x, const char *name, ASTNode **stmts, int n, int label) {
    int save_frame = x->frame;
    int save_locals = x->locals_used;
    int save_temps = x->temp_depth;
    int save_ctrl = x->nctrl;
    int save_epi = x->epilogue;
    int save_nlocals = g_nlocals;
    int save_nscope = g_nscope;

    int locals = 0;
    for (int i = 0; i < n; i++) locals += count_locals(x, stmts[i]);
    int frame = align16(locals + 8 * XV_MAX_TEMP);

    x->frame = frame;
    x->locals_used = 0;
    x->temp_depth = 0;
    x->nctrl = 0;
    x->epilogue = new_label(x);
    g_nlocals = 0;
    g_nscope = 0;
    scope_push();

    place_label(x, label);
    e8(x, 0x55);
    e8(x, 0x48); e8(x, 0x89); e8(x, 0xE5);
    e8(x, 0x48); e8(x, 0x81); e8(x, 0xEC);
    e32(x, frame);

    for (int i = 0; i < n; i++) gen_stmt(x, stmts[i]);

    i_mov_rax_imm(x, 0);
    place_label(x, x->epilogue);
    e8(x, 0xC9);
    e8(x, 0xC3);
    (void)name;

    g_nscope = save_nscope;
    g_nlocals = save_nlocals;
    x->frame = save_frame;
    x->locals_used = save_locals;
    x->temp_depth = save_temps;
    x->nctrl = save_ctrl;
    x->epilogue = save_epi;
}

/* ─────────────────────────────────────────────
   Collection passes
   ───────────────────────────────────────────── */
static void collect_structs(X *x, ASTNode *n) {
    if (!n) return;
    if (n->kind == NODE_STRUCT_DECL) {
        if (g_nstruct >= XV_MAX_STRUCT) { xfail(x, n->line, "too many struct types"); return; }
        Type *st = calloc(1, sizeof(Type));
        if (!st) { fputs("luc: out of memory\n", stderr); exit(1); }
        st->kind = TY_STRUCT;
        snprintf(st->name, sizeof st->name, "%s", n->sval ? n->sval : "_struct");
        int off = 0, max_align = 1;
        for (int i = 0; i < n->children.count; i++) {
            ASTNode *fnode = n->children.items[i];
            if (fnode->kind != NODE_VAR_DECL && fnode->kind != NODE_PTR_DECL) continue;
            Type *ft = (fnode->kind == NODE_PTR_DECL)
                       ? mk_ptr(type_from_name(x, fnode->type_name ? fnode->type_name : "byte"))
                       : type_from_name(x, fnode->type_name);
            int count = 0;
            int fsize = ft->size < 1 ? 1 : ft->size;
            if (fnode->ival > 0) {
                int64_t cv = 1;
                if (fnode->children.count > 0) const_eval_expr(fnode->children.items[0], &cv);
                if (cv < 1) cv = 1;
                count = (int)cv;
                fsize *= count;
            }
            int fal = fsize >= 8 ? 8 : fsize;
            if (fal > max_align) max_align = fal;
            off = (off + fal - 1) / fal * fal;
            if (st->nfields >= 32) { xfail(x, n->line, "struct '%s' has too many fields", st->name); break; }
            Field *fd = &st->fields[st->nfields++];
            snprintf(fd->name, sizeof fd->name, "%s", fnode->sval ? fnode->sval : "_f");
            fd->off = off;
            fd->ty = ft;
            fd->count = count;
            off += fsize;
        }
        st->size = align8(((off + max_align - 1) / max_align) * max_align);
        if (st->size <= 0) st->size = 8;
        g_structs[g_nstruct++] = st;
        return;
    }
    if (n->kind == NODE_FUNC_DECL || n->kind == NODE_ASYNC_FUNC) return;
    for (int i = 0; i < n->children.count; i++)
        collect_structs(x, n->children.items[i]);
}

static void collect_enums(X *x, ASTNode *n) {
    if (!n) return;
    if (n->kind == NODE_ENUM_DECL) {
        int64_t next = 0;
        for (int i = 0; i < n->children.count; i++) {
            ASTNode *m = n->children.items[i];
            if (m->kind != NODE_IDENT) continue;
            if (m->bval) next = m->ival;
            add_enum(x, m->sval, next);
            next++;
        }
        return;
    }
    if (n->kind == NODE_FUNC_DECL || n->kind == NODE_ASYNC_FUNC) return;
    for (int i = 0; i < n->children.count; i++)
        collect_enums(x, n->children.items[i]);
}

static void collect_funcs(X *x, ASTNode *n) {
    if (!n) return;
    if (n->kind == NODE_FUNC_DECL || n->kind == NODE_ASYNC_FUNC) {
        if (!n->sval) return;
        if (n->kind == NODE_ASYNC_FUNC) {
            xfail(x, n->line, "async functions need `luc -c`");
            return;
        }
        Fn *f = add_fn(x, n->sval);
        f->ret = (n->type_name && *n->type_name) ? type_from_name(x, n->type_name) : &ty_void;
        int np = 0;
        for (int i = 0; i < n->children.count && np < 6; i++) {
            ASTNode *c = n->children.items[i];
            if (c->kind == NODE_PROGRAM) break;
            f->params[np++] = fn_param_type(x, c);
        }
        f->nparams = np;
        f->defined = true;
        return;
    }
    for (int i = 0; i < n->children.count; i++)
        collect_funcs(x, n->children.items[i]);
}

static void collect_globals(X *x, ASTNode *n) {
    if (!n) return;
    if (n->kind == NODE_VAR_DECL || n->kind == NODE_PTR_DECL) {
        /* Only simple, constant-initialised declarations become globals. */
        if (n->ival > 1) return;
        Type *t = (n->kind == NODE_PTR_DECL)
                  ? mk_ptr(type_from_name(x, n->type_name ? n->type_name : "byte"))
                  : type_from_name(x, n->type_name);
        if (!t || t->kind == TY_VOID || t->kind == TY_FLOAT) return;

        int64_t count = 0;
        bool is_array = n->ival == 1;
        if (is_array) {
            if (n->children.count < 1) return;
            if (!const_eval_expr(n->children.items[0], &count)) return;
            if (count < 0) return;
        }

        int bytes = is_array ? (int)(count * t->size) : (t->size < 8 ? 8 : t->size);
        if (bytes <= 0) return;

        /* Validate initialisers before committing the storage. */
        int ninit = is_array ? n->children.count - 1 : n->children.count;
        for (int i = 0; i < ninit; i++) {
            const char *s = NULL;
            if (!is_array) {
                if (n->children.items[0]->kind == NODE_LITERAL_STR) s = n->children.items[0]->sval;
                else {
                    int64_t v;
                    if (!const_eval_expr(n->children.items[0], &v)) return;
                }
            } else {
                int64_t v;
                if (!const_eval_expr(n->children.items[1 + i], &v)) return;
            }
            (void)s;
        }

        int off = data_alloc(x, align8(bytes));
        if (g_nglobals >= 512) { xfail(x, n->line, "too many globals"); return; }
        Var *v = &g_globals[g_nglobals++];
        memset(v, 0, sizeof *v);
        snprintf(v->name, sizeof v->name, "%s", n->sval ? n->sval : "_g");
        v->ty = t;
        v->is_array = is_array;
        v->count = (int)count;
        v->off = off;
        v->global = true;

        if (is_array) {
            for (int i = 0; i < ninit && i < count; i++) {
                int64_t val = 0;
                const_eval_expr(n->children.items[1 + i], &val);
                if (val == 0) continue;
                memcpy(x->data.p + off + i * t->size, &val, (size_t)(t->size < 8 ? t->size : 8));
            }
        } else if (ninit > 0) {
            ASTNode *init = n->children.items[0];
            if (init->kind == NODE_LITERAL_STR) {
                int soff = data_str(x, init->sval ? init->sval : "");
                add_gdfx(x, off, soff);   /* patched to an absolute data address */
            } else {
                int64_t val = 0;
                const_eval_expr(init, &val);
                memcpy(x->data.p + off, &val, (size_t)(t->size < 8 ? t->size : 8));
            }
        }
        return;
    }
    if (n->kind == NODE_FUNC_DECL || n->kind == NODE_ASYNC_FUNC) return;
    for (int i = 0; i < n->children.count; i++)
        collect_globals(x, n->children.items[i]);
}

/* ─────────────────────────────────────────────
   ELF writer
   ───────────────────────────────────────────── */
static void put16(unsigned char *p, int off, uint16_t v) { memcpy(p + off, &v, 2); }
static void put32(unsigned char *p, int off, uint32_t v) { memcpy(p + off, &v, 4); }
static void put64(unsigned char *p, int off, uint64_t v) { memcpy(p + off, &v, 8); }

static void phdr(unsigned char *p, int po, uint32_t type, uint32_t flags,
                 uint64_t off, uint64_t vaddr, uint64_t filesz,
                 uint64_t memsz, uint64_t align) {
    put32(p, po + 0,  type);
    put32(p, po + 4,  flags);
    put64(p, po + 8,  off);
    put64(p, po + 16, vaddr);
    put64(p, po + 24, vaddr);
    put64(p, po + 32, filesz);
    put64(p, po + 40, memsz);
    put64(p, po + 48, align);
}

static bool write_elf(X *x, const char *path, int entry_off) {
    int text_len = x->code.len;
    int data_off = (XV_CODE_OFF + text_len + XV_PAGE - 1) / XV_PAGE * XV_PAGE;
    int data_len = x->data.len;
    if (data_len <= 0) data_len = 8;
    int total = data_off + data_len;

    unsigned char *img = calloc(1, (size_t)total);
    if (!img) { fputs("luc: out of memory\n", stderr); return false; }

    img[0] = 0x7F; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[4] = 2;    /* ELFCLASS64 */
    img[5] = 1;    /* little endian */
    img[6] = 1;    /* version */
    put16(img, 16, 2);        /* ET_EXEC */
    put16(img, 18, 0x3E);     /* EM_X86_64 */
    put32(img, 20, 1);
    put64(img, 24, XV_CODE_VA + (uint64_t)entry_off);
    put64(img, 32, 64);       /* e_phoff */
    put64(img, 40, 0);        /* e_shoff */
    put32(img, 48, 0);
    put16(img, 52, 64);
    put16(img, 54, 56);
    put16(img, 56, 3);
    put16(img, 58, 0);
    put16(img, 60, 0);
    put16(img, 62, 0);

    uint64_t data_va = XV_BASE + (uint64_t)data_off;
    phdr(img, 64,      1, 5, 0, XV_BASE, (uint64_t)XV_CODE_OFF + (uint64_t)text_len,
         (uint64_t)XV_CODE_OFF + (uint64_t)text_len, XV_PAGE);
    phdr(img, 64 + 56, 1, 6, (uint64_t)data_off, data_va,
         (uint64_t)data_len, (uint64_t)data_len, XV_PAGE);
    phdr(img, 64 + 112, 0x6474E551, 6, 0, 0, 0, 0, 16);

    if (text_len > 0) memcpy(img + XV_CODE_OFF, x->code.p, (size_t)text_len);
    if (x->data.len > 0) memcpy(img + data_off, x->data.p, (size_t)x->data.len);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[luc] failed to write output '%s'\n", path);
        free(img);
        return false;
    }
    size_t wrote = fwrite(img, 1, (size_t)total, f);
    bool ok = (wrote == (size_t)total) && (fclose(f) == 0);
    free(img);
    if (!ok) {
        fprintf(stderr, "[luc] failed to write output '%s'\n", path);
        return false;
    }
    chmod(path, 0755);
    return true;
}

/* ─────────────────────────────────────────────
   The Lu runtime prelude
   ───────────────────────────────────────────── */
static const char *LU_PRELUDE =
"fn lu_strlen(s: ptr/byte) -> int64 {\n"
"    var i: int64 = 0\n"
"    while s[i] != 0 {\n"
"        i = i + 1\n"
"    }\n"
"    return i\n"
"}\n"
"\n"
"fn lu_write(fd: int64, buf: ptr/byte, n: int64) -> int64 {\n"
"    return syscall(1, fd, buf, n)\n"
"}\n"
"\n"
"fn lu_puts(s: ptr/byte) -> void {\n"
"    if s == 0 {\n"
"        return\n"
"    }\n"
"    lu_write(1, s, lu_strlen(s))\n"
"}\n"
"\n"
"fn lu_putc(ch: int64) -> void {\n"
"    var b: byte[1]\n"
"    b[0] = ch\n"
"    lu_write(1, &b[0], 1)\n"
"}\n"
"\n"
"fn lu_putn(v: int64) -> void {\n"
"    var buf: byte[32]\n"
"    var i: int64 = 32\n"
"    var neg: bool = false\n"
"    if v < 0 {\n"
"        neg = true\n"
"        v = 0 - v\n"
"    }\n"
"    if v == 0 {\n"
"        i = i - 1\n"
"        buf[i] = 48\n"
"    }\n"
"    while v > 0 {\n"
"        i = i - 1\n"
"        buf[i] = 48 + (v % 10)\n"
"        v = v / 10\n"
"    }\n"
"    if neg {\n"
"        i = i - 1\n"
"        buf[i] = 45\n"
"    }\n"
"    lu_write(1, &buf[i], 32 - i)\n"
"}\n"
"\n"
"fn lu_memset(p: ptr/byte, v: int64, n: int64) -> void {\n"
"    var i: int64 = 0\n"
"    while i < n {\n"
"        p[i] = v\n"
"        i = i + 1\n"
"    }\n"
"}\n"
"\n"
"fn lu_memcpy(d: ptr/byte, s: ptr/byte, n: int64) -> void {\n"
"    var i: int64 = 0\n"
"    while i < n {\n"
"        d[i] = s[i]\n"
"        i = i + 1\n"
"    }\n"
"}\n"
"\n"
"fn lu_alloc(n: int64) -> ptr/byte {\n"
"    var p: int64 = syscall(9, 0, n, 3, 34, 0 - 1, 0)\n"
"    return p\n"
"}\n"
"\n"
"fn lu_free(p: ptr/byte) -> void {\n"
"    return\n"
"}\n";

/* ─────────────────────────────────────────────
   Prelude merge
   ───────────────────────────────────────────── */
static bool merge_prelude(X *x, ASTNode *root) {
    Lexer *lx = lexer_new(LU_PRELUDE);
    int tokc = 0;
    Token *toks = lexer_tokenize(lx, &tokc);
    lexer_free(lx);
    if (!toks) { xfail(x, 0, "cannot tokenise the runtime prelude"); return false; }
    ASTNode *pre = parse(toks, tokc);
    tokens_free(toks, tokc);
    if (!pre) { xfail(x, 0, "cannot parse the runtime prelude"); return false; }
    for (int i = 0; i < pre->children.count; i++)
        node_list_add(&root->children, pre->children.items[i]);
    free(pre->children.items);
    free(pre);
    return true;
}

/* ─────────────────────────────────────────────
   Public entry point
   ───────────────────────────────────────────── */
bool native_compile(ASTNode *root, const char *out_path, const char *src_name) {
    if (!root || !out_path) return false;

    types_init();
    g_nstruct = g_nlocals = g_nscope = g_nglobals = g_nfns = g_nenums = 0;

    X x;
    memset(&x, 0, sizeof x);
    x.src = src_name;
    x.frame = 0;
    x.epilogue = -1;

    if (!merge_prelude(&x, root)) return false;
    if (x.failed) return false;

    collect_structs(&x, root);
    collect_enums(&x, root);
    collect_funcs(&x, root);
    collect_globals(&x, root);
    if (x.failed) return false;

    /* Runtime strings. */
    g_true_off  = data_str(&x, "true");
    g_false_off = data_str(&x, "false");
    g_nl_off    = data_str(&x, "\n");

    int start_lbl = new_label(&x);
    place_label(&x, start_lbl);   /* entry sits at offset 0 */

    Fn *user_main = find_fn("main");

    /* Split top-level statements into a synthetic function body. */
    ASTNode **top = calloc((size_t)root->children.count + 1, sizeof(ASTNode *));
    int ntop = 0;
    if (top) {
        for (int i = 0; i < root->children.count; i++) {
            ASTNode *c = root->children.items[i];
            switch (c->kind) {
            case NODE_FUNC_DECL:
            case NODE_ASYNC_FUNC:
            case NODE_STRUCT_DECL:
            case NODE_ENUM_DECL:
            case NODE_UNION_DECL:
            case NODE_LANG_DECL:
            case NODE_IMPORT:
            case NODE_INCLUDE:
            case NODE_MODE:
            case NODE_DEF_CONFIG:
            case NODE_OPT:
            case NODE_ANNOT:
            case NODE_DEFINE:
            case NODE_DEF_CONST:
                break;
            case NODE_VAR_DECL:
            case NODE_PTR_DECL:
                /* collected as globals when possible; skip duplicates */
                break;
            default:
                top[ntop++] = c;
                break;
            }
        }
    }

    int top_lbl = new_label(&x);

    /* _start: run top-level code, then user main, then exit */
    if (user_main) {
        i_call(&x, top_lbl);
        i_call(&x, user_main->label);
        i_mov_reg_reg(&x, 7, 0);
    } else {
        i_call(&x, top_lbl);
        i_mov_reg_reg(&x, 7, 0);
    }
    i_mov_reg_imm(&x, 0, 60);
    i_syscall(&x);

    gen_synthetic(&x, "__lu_top", top, ntop, top_lbl);
    free(top);

    /* User functions */
    for (int i = 0; i < root->children.count; i++) {
        ASTNode *c = root->children.items[i];
        if (c->kind == NODE_FUNC_DECL) gen_function(&x, c);
    }

    if (x.failed) return false;

    /* Patch code-relative fixups. */
    for (int i = 0; i < x.ncfix; i++) {
        int at = x.cfix[i].at;
        int lbl = x.cfix[i].lbl;
        int32_t rel;
        if (lbl < 0 || lbl >= x.nlabels) rel = 0;
        else rel = (int32_t)(x.label_pos[lbl] - (at + 4));
        memcpy(x.code.p + at, &rel, 4);
    }

    /* Patch data references (RIP-relative). */
    int data_off = (XV_CODE_OFF + x.code.len + XV_PAGE - 1) / XV_PAGE * XV_PAGE;
    uint64_t data_va = XV_BASE + (uint64_t)data_off;
    for (int i = 0; i < x.ndfix; i++) {
        int at = x.dfix[i].at;
        int doff = x.dfix[i].doff;
        int32_t rel = (int32_t)((int64_t)(data_va + (uint64_t)doff) -
                                (int64_t)(XV_CODE_VA + (uint64_t)at + 4));
        memcpy(x.code.p + at, &rel, 4);
    }

    /* Patch compile-time pointer initialisers inside .data. */
    for (int i = 0; i < x.ngdfix; i++) {
        uint64_t val = data_va + (uint64_t)x.gdfix[i].doff;
        memcpy(x.data.p + x.gdfix[i].at, &val, sizeof val);
    }

    bool ok = write_elf(&x, out_path, x.label_pos[start_lbl]);

    free(x.code.p);
    free(x.data.p);
    return ok;
}
