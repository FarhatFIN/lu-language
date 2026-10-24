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

#if defined(__SIZEOF_INT128__)
/* The constant-divisor magic needs a 128-bit intermediate. */
__extension__ typedef unsigned __int128 u128;
#endif

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
    int   reg;       /* 0 = memory; else a callee-saved x86 register */
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
/* typed load into an arbitrary register: reg = [rbp-off] */
static void i_load_slot_reg(X *x, int reg, int disp, Type *t) {
    if (t->kind == TY_BOOL || t->kind == TY_BYTE) {
        rex_nw(x, reg, 5);
        e8(x, 0x0F); e8(x, 0xB6); modrm_mem(x, reg, 5, -disp);
    } else if (t->size == 8) {
        rex_w(x, reg, 5); e8(x, 0x8B); modrm_mem(x, reg, 5, -disp);
    } else {
        rex_w(x, reg, 5); e8(x, 0x63); modrm_mem(x, reg, 5, -disp);
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

/* rax ← rax OP imm. The immediate forms keep one-operand expressions such
   as `i + 1` from round-tripping through rcx. */
static bool imm32_ok(int64_t v) { return v >= -2147483648LL && v <= 2147483647LL; }
static bool imm8_ok(int64_t v)  { return v >= -128 && v <= 127; }

static void i_alu_rax_imm(X *x, int digit, int opcode32, int64_t v) {
    if (imm8_ok(v)) {
        e8(x, 0x48); e8(x, 0x83); e8(x, 0xC0 | (digit << 3)); e8(x, (int)(v & 0xFF));
    } else {
        e8(x, 0x48); e8(x, opcode32); e32(x, (int32_t)v);
    }
}
static void i_add_rax_imm(X *x, int64_t v) { i_alu_rax_imm(x, 0, 0x05, v); }
static void i_sub_rax_imm(X *x, int64_t v) { i_alu_rax_imm(x, 5, 0x2D, v); }
static void i_and_rax_imm(X *x, int64_t v) { i_alu_rax_imm(x, 4, 0x25, v); }
static void i_or_rax_imm(X *x, int64_t v)  { i_alu_rax_imm(x, 1, 0x0D, v); }
static void i_xor_rax_imm(X *x, int64_t v) { i_alu_rax_imm(x, 6, 0x35, v); }
static void i_cmp_rax_imm(X *x, int64_t v) { i_alu_rax_imm(x, 7, 0x3D, v); }

/* imul rax, rax, imm */
static void i_imul_rax_imm(X *x, int64_t v) {
    if (imm8_ok(v)) {
        e8(x, 0x48); e8(x, 0x6B); e8(x, 0xC0); e8(x, (int)(v & 0xFF));
    } else {
        e8(x, 0x48); e8(x, 0x69); e8(x, 0xC0); e32(x, (int32_t)v);
    }
}
/* shift by an immediate: /4 shl, /5 shr, /7 sar */
static void i_shift_rax_imm(X *x, int digit, int64_t v) {
    e8(x, 0x48); e8(x, 0xC1); e8(x, 0xC0 | (digit << 3)); e8(x, (int)(v & 0x3F));
}
static void i_shl_rax_imm(X *x, int64_t v) { i_shift_rax_imm(x, 4, v); }
static void i_shr_rax_imm(X *x, int64_t v) { i_shift_rax_imm(x, 5, v); }
static void i_sar_rax_imm(X *x, int64_t v) { i_shift_rax_imm(x, 7, v); }

/* lea rax, [rcx + rax*scale]; scale must be 1, 2, 4 or 8. */
static void i_lea_rax_rcx_index(X *x, int scale) {
    int sb = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    e8(x, 0x48); e8(x, 0x8D); e8(x, 0x04); e8(x, (sb << 6) | 0x01);
}

/* lea rax, [rax + rcx*scale]; scale must be 1, 2, 4 or 8. */
static void i_lea_rax_base_rcx(X *x, int scale) {
    int sb = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    e8(x, 0x48); e8(x, 0x8D); e8(x, 0x04); e8(x, (sb << 6) | 0x08);
}

/* lea rax, [rax + idxreg*scale]. rsp cannot appear as an SIB index,
   and r12 encodes as rsp there, so callers must not pass it. */
static void i_lea_rax_reg_index(X *x, int idxreg, int scale) {
    int sb = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    e8(x, 0x48 | ((idxreg & 8) ? 2 : 0));
    e8(x, 0x8D); e8(x, 0x04);
    e8(x, (sb << 6) | ((idxreg & 7) << 3));
}

/* lea rcx, [base + idxreg*scale]. rsp/r12 cannot appear as an SIB index,
   so the index register must not be one. The base may be any register:
   rsp/r12 need the disp32 form and rbp/r13 the disp8 form, because their
   encodings in the base field otherwise mean "no base". */
static void i_lea_rcx_regs(X *x, int base, int idxreg, int scale) {
    int sb = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    int mod = (base & 7) == 4 ? 2 : (base & 7) == 5 ? 1 : 0;
    e8(x, 0x48 | ((idxreg & 8) ? 2 : 0) | ((base & 8) ? 1 : 0));
    e8(x, 0x8D);
    e8(x, (mod << 6) | 0x0C);
    e8(x, (sb << 6) | ((idxreg & 7) << 3) | (base & 7));
    if (mod == 2) e32(x, 0);
    else if (mod == 1) e8(x, 0);
}

/* dst OP= [base + idxreg*scale], 64-bit. rsp/r12 cannot be an SIB index;
   the base may be any register (rsp/r12 use disp32, rbp/r13 use disp8). */
static void i_alu_reg_mem_sib(X *x, int digit, int dst, int base, int idxreg, int scale) {
    int sb = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    int mod = (base & 7) == 4 ? 2 : (base & 7) == 5 ? 1 : 0;
    e8(x, 0x48 | ((dst & 8) ? 4 : 0) | ((idxreg & 8) ? 2 : 0) | ((base & 8) ? 1 : 0));
    e8(x, digit ? 0x2B : 0x03);              /* sub / add r64, r/m64 */
    e8(x, (mod << 6) | ((dst & 7) << 3) | 0x04);
    e8(x, (sb << 6) | ((idxreg & 7) << 3) | (base & 7));
    if (mod == 2) e32(x, 0);
    else if (mod == 1) e8(x, 0);
}

/* dst OP= [base + disp32], 64-bit. */
static void i_alu_reg_mem_disp(X *x, int digit, int dst, int base, int32_t disp) {
    e8(x, 0x48 | ((dst & 8) ? 4 : 0) | ((base & 8) ? 1 : 0));
    e8(x, digit ? 0x2B : 0x03);
    if ((base & 7) == 4) {
        e8(x, 0x84);                          /* mod=10, rm=100 (SIB) */
        e8(x, 0x24);                          /* no index, base rsp/r12 */
    } else {
        e8(x, 0x80 | (base & 7));
    }
    e32(x, disp);
}

/* mov r64, imm64 (movabs) */
static void i_mov_reg_imm64(X *x, int reg, uint64_t v) {
    rex_w(x, 0, reg);
    e8(x, 0xB8 + (reg & 7));
    e64(x, (int64_t)v);
}
/* dst OP= src, both 64-bit */
static void i_xor_reg_reg(X *x, int dst, int src) {
    rex_w(x, src, dst); e8(x, 0x31); modrm_rr(x, src, dst);
}
static void i_sub_reg_reg(X *x, int dst, int src) {
    rex_w(x, src, dst); e8(x, 0x29); modrm_rr(x, src, dst);
}
static void i_add_reg_reg(X *x, int dst, int src) {
    rex_w(x, src, dst); e8(x, 0x01); modrm_rr(x, src, dst);
}
/* reg OP= imm32 */
static void i_alu_reg_imm(X *x, int op, int reg, int64_t v) {
    rex_w(x, 0, reg);
    if (imm8_ok(v)) { e8(x, 0x83); modrm_rr(x, op, reg); e8(x, v & 0xFF); }
    else            { e8(x, 0x81); modrm_rr(x, op, reg); e32(x, (int32_t)v); }
}
static void i_and_reg_imm(X *x, int reg, int64_t v) { i_alu_reg_imm(x, 4, reg, v); }
static void i_inc_reg(X *x, int reg) { rex_w(x, 0, reg); e8(x, 0xFF); modrm_rr(x, 0, reg); }
static void i_dec_reg(X *x, int reg) { rex_w(x, 0, reg); e8(x, 0xFF); modrm_rr(x, 1, reg); }
static void i_neg_reg(X *x, int reg) { rex_w(x, 0, reg); e8(x, 0xF7); modrm_rr(x, 3, reg); }
static void i_sar_reg_imm(X *x, int reg, int imm) {
    rex_w(x, 0, reg); e8(x, 0xC1); modrm_rr(x, 7, reg); e8(x, imm & 0x3F);
}
static void i_not_reg(X *x, int reg) {
    rex_w(x, 0, reg); e8(x, 0xF7); modrm_rr(x, 2, reg);
}
/* unsigned rdx:rax = rax * src */
static void i_mul_reg(X *x, int src) {
    rex_w(x, 0, src); e8(x, 0xF7); modrm_rr(x, 4, src);
}
/* signed rdx:rax = rax * src */
static void i_imul_rax_reg(X *x, int src) {
    rex_w(x, 0, src); e8(x, 0xF7); modrm_rr(x, 5, src);
}
/* imul dst, dst, imm32 */
static void i_imul_reg_imm(X *x, int reg, int64_t v) {
    rex_w(x, reg, reg); e8(x, 0x69); modrm_rr(x, reg, reg); e32(x, (int32_t)v);
}
/* flags for a - b */
static void i_cmp_reg_reg(X *x, int a, int b) {
    rex_w(x, b, a); e8(x, 0x39); modrm_rr(x, b, a);
}
/* flags for reg - imm32 */
static void i_cmp_reg_imm(X *x, int reg, int64_t v) {
    rex_w(x, 0, reg);
    if (imm8_ok(v)) { e8(x, 0x83); modrm_rr(x, 7, reg); e8(x, v & 0xFF); }
    else            { e8(x, 0x81); modrm_rr(x, 7, reg); e32(x, (int32_t)v); }
}
static void i_push_reg(X *x, int reg) {
    rex_nw(x, 0, reg);
    e8(x, 0x50 + (reg & 7));
}
static void i_pop_reg(X *x, int reg) {
    rex_nw(x, 0, reg);
    e8(x, 0x58 + (reg & 7));
}

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
/* Pad to a 16-byte boundary before a hot loop head. The image starts at
   XV_CODE_VA, which is itself 16-byte aligned, so the code offset is the
   in-page alignment. */
static void align_loop(X *x) {
    while (x->code.len % 16) e8(x, 0x90);
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

/* mod=00, rm=101: RIP-relative operand, displacement fixed up later. */
static void modrm_rip(X *x, int reg, int doff) {
    e8(x, ((reg & 7) << 3) | 5);
    int at = x->code.len; e32(x, 0);
    add_dfx(x, at, doff);
}

/* typed load into an arbitrary register from a global: reg = [rip+doff] */
static void i_load_data_reg(X *x, int reg, int doff, Type *t) {
    if (t->kind == TY_BOOL || t->kind == TY_BYTE) {
        rex_nw(x, reg, 0);
        e8(x, 0x0F); e8(x, 0xB6); modrm_rip(x, reg, doff);
    } else if (t->size == 8) {
        rex_w(x, reg, 0); e8(x, 0x8B); modrm_rip(x, reg, doff);
    } else {
        rex_w(x, reg, 0); e8(x, 0x63); modrm_rip(x, reg, doff);
    }
}
/* typed store from an arbitrary register to a global: [rip+doff] = reg */
static void i_store_data_reg(X *x, int doff, Type *t, int reg) {
    if (t->kind == TY_BOOL || t->kind == TY_BYTE) {
        rex_nw(x, reg, 0);
        e8(x, 0x88); modrm_rip(x, reg, doff);
    } else if (t->size == 8) {
        rex_w(x, reg, 0);
        e8(x, 0x89); modrm_rip(x, reg, doff);
    } else {
        rex_nw(x, reg, 0);
        e8(x, 0x89); modrm_rip(x, reg, doff);
    }
}

/* Read-modify-write on a variable slot, used for `x = x + 1` and friends.
   `global` selects the RIP-relative encoding; otherwise the slot is
   rbp-relative. */
static void i_alu_slot_imm(X *x, bool global, int doff, Type *t,
                           int digit, int64_t v) {
    int bytes = (t->kind == TY_BOOL || t->kind == TY_BYTE) ? 1
              : (t->size == 8 ? 8 : 4);
    if (bytes == 1) {
        e8(x, 0x80);
        if (global) modrm_rip(x, digit, doff);
        else modrm_mem(x, digit, 5, -doff);
        e8(x, (int)(v & 0xFF));
        return;
    }
    if (bytes == 8) e8(x, 0x48);
    if (imm8_ok(v)) {
        e8(x, 0x83);
        if (global) modrm_rip(x, digit, doff);
        else modrm_mem(x, digit, 5, -doff);
        e8(x, (int)(v & 0xFF));
    } else {
        e8(x, 0x81);
        if (global) modrm_rip(x, digit, doff);
        else modrm_mem(x, digit, 5, -doff);
        e32(x, (int32_t)v);
    }
}
static void i_inc_slot(X *x, bool global, int doff, Type *t) {
    int bytes = (t->kind == TY_BOOL || t->kind == TY_BYTE) ? 1
              : (t->size == 8 ? 8 : 4);
    if (bytes == 8) e8(x, 0x48);
    e8(x, bytes == 1 ? 0xFE : 0xFF);
    if (global) modrm_rip(x, 0, doff);
    else modrm_mem(x, 0, 5, -doff);
}
static void i_dec_slot(X *x, bool global, int doff, Type *t) {
    int bytes = (t->kind == TY_BOOL || t->kind == TY_BYTE) ? 1
              : (t->size == 8 ? 8 : 4);
    if (bytes == 8) e8(x, 0x48);
    e8(x, bytes == 1 ? 0xFE : 0xFF);
    if (global) modrm_rip(x, 1, doff);
    else modrm_mem(x, 1, 5, -doff);
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
static Var *push_var(const char *name, Type *ty, bool is_array, int count, int off, bool global) {
    if (g_nlocals >= XV_MAX_VAR) return NULL;
    Var *v = &g_locals[g_nlocals++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name ? name : "_v");
    v->ty = ty;
    v->is_array = is_array;
    v->count = count;
    v->off = off;
    v->global = global;
    return v;
}

/* ─────────────────────────────────────────────
   Static register allocation for scalar locals

   Up to four 8-byte locals per function live in callee-saved registers
   (r12–r15) for their whole lifetime. Callees preserve them, so calls
   need no spill; control-flow joins need no reconciliation. Address-taken
   variables stay in memory.
   ───────────────────────────────────────────── */
#define XV_MAX_CACHE 4
static const int CACHE_REGS[XV_MAX_CACHE] = { 12, 13, 14, 15 };

static Type *type_from_name(X *x, const char *n);

typedef struct { ASTNode *decl; int reg; } RegAssign;
static RegAssign g_regmap[128];
static int g_nregmap;
static int g_nextreg;

static bool scalar_reg_type(Type *t) {
    return t && t->size == 8 &&
           (t->kind == TY_I64 || t->kind == TY_PTR || t->kind == TY_STR);
}

static bool addr_taken_in(ASTNode *n, const char *name) {
    if (!n) return false;
    if (n->kind == NODE_EXPR_REF && n->children.count > 0) {
        ASTNode *c = n->children.items[0];
        if (c && c->kind == NODE_IDENT && c->sval && !strcmp(c->sval, name))
            return true;
    }
    for (int i = 0; i < n->children.count; i++)
        if (addr_taken_in(n->children.items[i], name)) return true;
    return false;
}

static int reg_for_node(ASTNode *n) {
    for (int i = 0; i < g_nregmap; i++)
        if (g_regmap[i].decl == n) return g_regmap[i].reg;
    return 0;
}

static int cache_add(ASTNode *decl) {
    if (g_nregmap >= (int)(sizeof g_regmap / sizeof g_regmap[0])) return 0;
    if (g_nextreg >= XV_MAX_CACHE) return 0;
    int reg = CACHE_REGS[g_nextreg++];
    g_regmap[g_nregmap].decl = decl;
    g_regmap[g_nregmap].reg = reg;
    g_nregmap++;
    return reg;
}

static void cache_walk(X *x, ASTNode *n, ASTNode *body) {
    if (!n) return;
    if ((n->kind == NODE_VAR_DECL || n->kind == NODE_PTR_DECL) &&
        n->sval && n->ival == 0) {
        Type *t = NULL;
        if (n->kind == NODE_PTR_DECL) {
            t = mk_ptr(type_from_name(x, n->type_name ? n->type_name : "byte"));
        } else if (n->type_name && strcmp(n->type_name, "auto") != 0) {
            t = type_from_name(x, n->type_name);
        }
        if (scalar_reg_type(t) && !addr_taken_in(body, n->sval))
            cache_add(n);
    }
    for (int i = 0; i < n->children.count; i++)
        cache_walk(x, n->children.items[i], body);
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
static bool leaf_to_reg(X *x, ASTNode *n, int reg);

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
        ASTNode *idx = n->children.items[1];
        Type *bt = etype(x, base);
        Type *elem = is_ptr_like(bt) ? (bt->base ? bt->base : &ty_byte) : &ty_int;
        int esz = elem->size > 0 ? elem->size : 1;

        int t = x->temp_depth++;
        /* A simple base loads without touching rcx, so a leaf index can
           go straight into the SIB and skip the address temporary. */
        if (base->kind == NODE_IDENT && find_var(base->sval) &&
            (esz == 1 || esz == 2 || esz == 4 || esz == 8)) {
            Var *iv = idx->kind == NODE_IDENT ? find_var(idx->sval) : NULL;
            if (iv && iv->reg && !iv->is_array && (iv->reg & 7) != 4) {
                gen_addr_base(x, base);
                if (x->failed) { x->temp_depth = t; return; }
                i_lea_rax_reg_index(x, iv->reg, esz);
                x->temp_depth = t;
                return;
            }
            if (leaf_to_reg(x, idx, 1)) {
                gen_addr_base(x, base);
                if (x->failed) { x->temp_depth = t; return; }
                i_lea_rax_base_rcx(x, esz);
                x->temp_depth = t;
                return;
            }
        }

        gen_addr_base(x, base);
        if (x->failed) { x->temp_depth = t; return; }
        temp_store(x, t);

        gen_expr(x, idx);
        if (x->failed) { x->temp_depth = t; return; }
        if (esz == 1 || esz == 2 || esz == 4 || esz == 8) {
            /* rax = index, rcx = base; one LEA with a scale factor. */
            temp_load_rcx(x, t);
            i_lea_rax_rcx_index(x, esz);
        } else {
            i_mov_reg_reg(x, 1, 0);           /* rcx = index */
            i_mov_rax_imm(x, esz);            /* rax = elem size */
            i_imul(x);                        /* rax = index * size */
            temp_load_rcx(x, t);              /* rcx = base */
            e8(x, 0x48); e8(x, 0x01); e8(x, 0xC1); /* add rcx, rax */
            i_mov_reg_reg(x, 0, 1);           /* rax = rcx */
        }
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

/* Condition code for `lhs OP rhs`, matching i_cmp_rax_rcx's operand order. */
static int cmp_cc(const char *op) {
    if (!strcmp(op, "==")) return CC_E;
    if (!strcmp(op, "!=")) return CC_NE;
    if (!strcmp(op, "<"))  return CC_L;
    if (!strcmp(op, "<=")) return CC_LE;
    if (!strcmp(op, ">"))  return CC_G;
    return CC_GE;
}
static int cc_inv(int cc) {
    switch (cc) {
    case CC_E:  return CC_NE;
    case CC_NE: return CC_E;
    case CC_L:  return CC_GE;
    case CC_GE: return CC_L;
    case CC_LE: return CC_G;
    case CC_G:  return CC_LE;
    case CC_B:  return CC_AE;
    case CC_AE: return CC_B;
    default:    return cc;
    }
}

/* True when `n` is a literal that fits a sign-extended imm32. */
static bool leaf_imm(ASTNode *n, int64_t *out) {
    if (!n) return false;
    if (n->kind == NODE_LITERAL_INT) {
        if (!imm32_ok(n->ival)) return false;
        *out = n->ival;
        return true;
    }
    if (n->kind == NODE_LITERAL_BOOL) {
        *out = n->bval ? 1 : 0;
        return true;
    }
    return false;
}

/* Materialise a side-effect-free operand into `reg` without touching rax
   and without control flow. Enum constants are left to gen_expr. */
static bool leaf_to_reg(X *x, ASTNode *n, int reg) {
    if (!n) return false;
    if (n->kind == NODE_LITERAL_INT) {
        if (!imm32_ok(n->ival)) return false;
        i_mov_reg_imm(x, reg, n->ival);
        return true;
    }
    if (n->kind == NODE_LITERAL_BOOL) {
        i_mov_reg_imm(x, reg, n->bval ? 1 : 0);
        return true;
    }
    if (n->kind == NODE_IDENT) {
        Var *v = find_var(n->sval);
        if (!v || v->is_array) return false;
        if (v->reg) i_mov_reg_reg(x, reg, v->reg);
        else if (v->global) i_load_data_reg(x, reg, v->off, v->ty);
        else i_load_slot_reg(x, reg, v->off, v->ty);
        return true;
    }
    return false;
}

/* Round-up magic for signed division by a positive constant.
   With p = 64 + shift and M = floor(2^p / d) + 1, the error
   e = M*d - 2^p must satisfy 1 <= e <= 2^(shift+1). Since |n| <= 2^63,
   n*e/(d*2^p) <= 1/d, so the fractional parts of n/d and of n*M/2^p
   sum to at most 1 and the multiply-high quotient is exact for both
   signs. |d| must be >= 2 (powers of two are shifted instead). */
static bool signed_magic(uint64_t d, int *shift, uint64_t *magic) {
#if defined(__SIZEOF_INT128__)
    for (int s = 0; s <= 62; s++) {
        u128 p2 = ((u128)1) << (64 + s);
        u128 m = p2 / (u128)d + 1;
        if (m >= ((u128)1 << 64)) continue;
        u128 e = m * (u128)d - p2;
        if (e == 0 || e > (((u128)1) << (s + 1))) continue;
        *shift = s;
        *magic = (uint64_t)m;
        return true;
    }
#endif
    return false;
}

/* Exact signed `rax / d` and `rax % d` for a compile-time divisor.
   Returns false when no cheap sequence applies, so the caller can fall
   back to idiv. */
static bool gen_const_divmod(X *x, int64_t d, bool want_rem) {
    uint64_t dabs = d < 0 ? (uint64_t)(-(d + 1)) + 1 : (uint64_t)d;
    if (dabs < 2) return false;

    bool pow2 = (dabs & (dabs - 1)) == 0;
    int shift = 0;
    uint64_t magic = 0;
    if (pow2) {
        while (((uint64_t)1 << shift) < dabs) shift++;
    } else if (!signed_magic(dabs, &shift, &magic)) {
        return false;
    }

    i_mov_reg_reg(x, 8, 0);                  /* r8 = n */
    i_mov_reg_reg(x, 9, 0);
    i_sar_reg_imm(x, 9, 63);                 /* r9 = sign(n) */

    if (pow2) {
        /* trunc(n / 2^k) = (n + (n<0 ? 2^k-1 : 0)) >> k */
        i_and_reg_imm(x, 9, (int64_t)(dabs - 1));
        i_add_reg_reg(x, 0, 9);
        i_sar_reg_imm(x, 0, shift);
    } else {
        i_mov_reg_imm64(x, 11, magic);
        i_imul_rax_reg(x, 11);               /* rdx = high half, signed */
        if (magic >> 63) i_add_reg_reg(x, 2, 8);  /* undo the -n*2^64 */
        if (shift) i_sar_reg_imm(x, 2, shift);
        i_mov_reg_reg(x, 0, 2);
        i_sub_reg_reg(x, 0, 9);              /* q -= sign(n) */
    }
    if (d < 0) i_neg_reg(x, 0);
    if (want_rem) {
        i_imul_rax_imm(x, d);                /* q * d */
        i_sub_reg_reg(x, 8, 0);              /* r8 = n - q*d */
        i_mov_reg_reg(x, 0, 8);
    }
    return true;
}

/* rax already holds the left operand. Computes `rax OP rhs`; `scale` is
   the pointer element size for pointer +/- (1 means no scaling). */
static void gen_binop_from_rax(X *x, const char *op, Type *lt,
                               ASTNode *rhs, int scale, int line) {
    int64_t iv;
    if (scale == 1 && leaf_imm(rhs, &iv)) {
        if ((!strcmp(op, "/") || !strcmp(op, "%")) && iv != 0 &&
            iv >= -2147483647LL && iv <= 2147483647LL &&
            gen_const_divmod(x, iv, !strcmp(op, "%"))) {
            return;
        }
        if (!strcmp(op, "+"))  { i_add_rax_imm(x, iv); return; }
        if (!strcmp(op, "-"))  { i_sub_rax_imm(x, iv); return; }
        if (!strcmp(op, "*"))  { i_imul_rax_imm(x, iv); return; }
        if (!strcmp(op, "&"))  { i_and_rax_imm(x, iv); return; }
        if (!strcmp(op, "|"))  { i_or_rax_imm(x, iv); return; }
        if (!strcmp(op, "^"))  { i_xor_rax_imm(x, iv); return; }
        if (!strcmp(op, "<<")) { i_shl_rax_imm(x, iv); return; }
        if (!strcmp(op, ">>")) {
            if (lt->kind == TY_I64 || lt->kind == TY_INT) i_sar_rax_imm(x, iv);
            else i_shr_rax_imm(x, iv);
            return;
        }
        if (is_cmp_op(op)) { i_cmp_rax_imm(x, iv); i_setcc(x, cmp_cc(op)); return; }
        /* / and % have no immediate form; fall through to rcx. */
    }
    if (scale == 1 && leaf_to_reg(x, rhs, 1)) {
        /* rcx already holds the right operand. */
    } else {
        int t = x->temp_depth++;
        temp_store(x, t);
        gen_expr(x, rhs);
        if (x->failed) { x->temp_depth = t; return; }
        if (scale != 1) {
            i_mov_reg_reg(x, 1, 0);
            i_mov_rax_imm(x, scale);
            i_imul(x);
        }
        i_mov_reg_reg(x, 1, 0);     /* rcx = rhs */
        temp_load_rax(x, t);        /* rax = lhs */
        x->temp_depth = t;
    }

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
    if (is_cmp_op(op)) { i_cmp_rax_rcx(x); i_setcc(x, cmp_cc(op)); return; }
    xfail(x, line, "operator '%s' is not supported by the native backend", op);
}

static void gen_binop(X *x, ASTNode *n) {
    if (n->children.count < 2) { xfail(x, n->line, "malformed binary expression"); return; }
    const char *op = n->op ? n->op : "+";
    ASTNode *lhs = n->children.items[0];
    ASTNode *rhs = n->children.items[1];

    /* short-circuit logic: `a && b` bails out to 0 on the first false
       operand, `a || b` to 1 on the first true one. */
    if (!strcmp(op, "&&") || !strcmp(op, "||")) {
        bool is_and = !strcmp(op, "&&");
        int lexit = new_label(x), lend = new_label(x);
        gen_expr(x, lhs);
        if (x->failed) return;
        i_test_rax(x);
        i_jcc(x, is_and ? CC_E : CC_NE, lexit);
        gen_expr(x, rhs);
        if (x->failed) return;
        i_test_rax(x);
        i_jcc(x, is_and ? CC_E : CC_NE, lexit);
        i_mov_rax_imm(x, is_and ? 1 : 0);
        i_jmp(x, lend);
        place_label(x, lexit);
        i_mov_rax_imm(x, is_and ? 0 : 1);
        place_label(x, lend);
        return;
    }

    Type *lt = etype(x, lhs);
    bool ptr_arith = is_ptr_like(lt) &&
                     (!strcmp(op, "+") || !strcmp(op, "-"));
    int scale = 1;
    if (ptr_arith)
        scale = (lt->base && lt->base->size > 0) ? lt->base->size : 1;

    int t = x->temp_depth++;
    gen_expr(x, lhs);
    if (x->failed) { x->temp_depth = t; return; }
    gen_binop_from_rax(x, op, lt, rhs, scale, n->line);
    x->temp_depth = t;
}

/* Jump to `target` if the condition evaluates as requested. Comparisons
   compile straight to flags + jcc instead of materialising 0/1 and
   testing it again; && and || short-circuit without temporaries. */
static void gen_cond_jump(X *x, ASTNode *n, int target, bool jump_if_true) {
    if (!n || x->failed) return;

    if (n->kind == NODE_EXPR_UNOP && n->op && !strcmp(n->op, "!") &&
        n->children.count > 0) {
        gen_cond_jump(x, n->children.items[0], target, !jump_if_true);
        return;
    }

    if (n->kind == NODE_EXPR_BINOP && n->op && n->children.count >= 2) {
        const char *op = n->op;
        bool is_and = !strcmp(op, "&&");
        bool is_or  = !strcmp(op, "||");
        if (is_and || is_or) {
            /* Both-jump form when the result requested is the short-circuit
               value: every operand that decides it jumps to target. */
            if (jump_if_true != is_and) {
                gen_cond_jump(x, n->children.items[0], target, jump_if_true);
                gen_cond_jump(x, n->children.items[1], target, jump_if_true);
            } else {
                int skip = new_label(x);
                gen_cond_jump(x, n->children.items[0], skip, !jump_if_true);
                gen_cond_jump(x, n->children.items[1], target, jump_if_true);
                place_label(x, skip);
            }
            return;
        }
        if (is_cmp_op(op)) {
            ASTNode *lhs = n->children.items[0];
            ASTNode *rhs = n->children.items[1];
            int t = x->temp_depth++;
            int64_t iv;
            int cc = cmp_cc(op);

            /* Compare a register-cached local in place. */
            Var *lv = lhs->kind == NODE_IDENT ? find_var(lhs->sval) : NULL;
            if (lv && lv->reg && !lv->is_array) {
                Var *rv = rhs->kind == NODE_IDENT ? find_var(rhs->sval) : NULL;
                if (leaf_imm(rhs, &iv)) {
                    i_cmp_reg_imm(x, lv->reg, iv);
                } else if (rv && rv->reg && !rv->is_array) {
                    i_cmp_reg_reg(x, lv->reg, rv->reg);
                } else {
                    gen_expr(x, rhs);
                    if (x->failed) { x->temp_depth = t; return; }
                    i_mov_reg_reg(x, 1, 0);
                    i_cmp_reg_reg(x, lv->reg, 1);
                }
                x->temp_depth = t;
                i_jcc(x, jump_if_true ? cc : cc_inv(cc), target);
                return;
            }

            gen_expr(x, lhs);
            if (x->failed) { x->temp_depth = t; return; }
            if (leaf_imm(rhs, &iv)) {
                i_cmp_rax_imm(x, iv);
            } else if (leaf_to_reg(x, rhs, 1)) {
                i_cmp_rax_rcx(x);
            } else {
                temp_store(x, t);
                gen_expr(x, rhs);
                if (x->failed) { x->temp_depth = t; return; }
                i_mov_reg_reg(x, 1, 0);
                temp_load_rax(x, t);
                i_cmp_rax_rcx(x);
            }
            x->temp_depth = t;
            i_jcc(x, jump_if_true ? cc : cc_inv(cc), target);
            return;
        }
    }

    gen_expr(x, n);
    if (x->failed) return;
    i_test_rax(x);
    i_jcc(x, jump_if_true ? CC_NE : CC_E, target);
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
    /* One argument cannot clobber another, so skip the temporary. */
    if (cnt == 1) {
        ASTNode *a = n->children.items[0];
        if (a && a->kind == NODE_EXPR_BINOP && a->op && a->children.count >= 2) {
            bool add = !strcmp(a->op, "+"), sub = !strcmp(a->op, "-");
            if (add || sub) {
                ASTNode *l = a->children.items[0], *r = a->children.items[1];
                Var *lv = l->kind == NODE_IDENT ? find_var(l->sval) : NULL;
                Var *rv = r->kind == NODE_IDENT ? find_var(r->sval) : NULL;
                int64_t k = 0;
                /* v +/- k straight into the argument register. Pointer
                   arithmetic is left to the general path, which scales. */
                if (lv && lv->reg && !lv->is_array && !is_ptr_like(lv->ty) &&
                    leaf_imm(r, &k) && imm32_ok(add ? k : -k)) {
                    i_mov_reg_reg(x, regs[0], lv->reg);
                    if (k) i_alu_reg_imm(x, add ? 0 : 5, regs[0], k);
                    return;
                }
                if (add && rv && rv->reg && !rv->is_array && !is_ptr_like(rv->ty) &&
                    leaf_imm(l, &k) && imm32_ok(k)) {
                    i_mov_reg_reg(x, regs[0], rv->reg);
                    if (k) i_alu_reg_imm(x, 0, regs[0], k);
                    return;
                }
            }
        }
        gen_expr(x, a);
        if (x->failed) return;
        i_mov_reg_reg(x, regs[0], 0);
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
        if (v->reg) {
            i_mov_reg_reg(x, 0, v->reg);
            return;
        }
        if (v->reg) {
            /* Address-taken locals are never cached, so this is only a
               safety net: spill first so the address sees the live value. */
            i_store_slot_reg(x, v->off, v->ty, v->reg);
            i_lea_mem(x, 0, v->off);
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
        gen_cond_jump(x, n->children.items[0], lelse, false);
        if (x->failed) return;
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

    Var *v = push_var(n->sval, t, false, 0, off, false);
    if (v) v->reg = reg_for_node(n);

    if (v && v->reg) {
        if (n->children.count > 0) {
            gen_expr(x, n->children.items[0]);
            if (x->failed) return;
            i_mov_reg_reg(x, v->reg, 0);
        }
        return;
    }

    if (t->kind == TY_STRUCT) zero_region(x, off, t->size);

    if (n->children.count > 0) {
        gen_expr(x, n->children.items[0]);
        if (x->failed) return;
        i_store_slot(x, off, t);
    }
}

/* True when evaluating `n` cannot assign, call or otherwise change memory. */
static bool expr_is_pure(ASTNode *n) {
    if (!n) return true;
    switch (n->kind) {
    case NODE_SET:
    case NODE_FUNC_CALL:
    case NODE_ALLOC:
    case NODE_FREE:
    case NODE_SEND:
    case NODE_RECV:
    case NODE_CHAN_SEND:
    case NODE_CHAN_RECV:
    case NODE_THROW:
    case NODE_AWAIT:
    case NODE_SPAWN:
        return false;
    default:
        break;
    }
    for (int i = 0; i < n->children.count; i++)
        if (!expr_is_pure(n->children.items[i])) return false;
    return true;
}

/* True for `x` itself inside `x = x + c`-shaped right-hand sides. */
static bool same_ident(ASTNode *n, const char *name) {
    return n && n->kind == NODE_IDENT && n->sval && name &&
           !strcmp(n->sval, name);
}

static void gen_set(X *x, ASTNode *n) {
    if (!n->lhs) {
        xfail(x, n->line, "assignment target could not be resolved natively "
                          "(use `luc -c` if this is a class field)");
        return;
    }
    if (n->children.count < 1) { xfail(x, n->line, "assignment without a value"); return; }

    const char *op = n->op;
    bool compound = op && *op;
    ASTNode *rhs = n->children.items[0];
    Var *lv = (n->lhs->kind == NODE_IDENT) ? find_var(n->lhs->sval) : NULL;

    /* x = x + c / x = c + x / x = x - c becomes a single read-modify-write;
       loop increments are the dominant case. */
    if (lv && !lv->is_array && !compound &&
        rhs->kind == NODE_EXPR_BINOP && rhs->op && rhs->children.count >= 2) {
        const char *bop = rhs->op;
        bool add = !strcmp(bop, "+");
        bool sub = !strcmp(bop, "-");
        if (add || sub) {
            ASTNode *a = rhs->children.items[0];
            ASTNode *b = rhs->children.items[1];
            ASTNode *other = NULL;
            int64_t iv;
            if (same_ident(a, lv->name)) other = b;
            else if (add && same_ident(b, lv->name)) other = a;
            if (other && lv->reg) {
                Var *rv = other->kind == NODE_IDENT ? find_var(other->sval) : NULL;
                if (rv && rv->reg && !rv->is_array) {
                    if (add) i_add_reg_reg(x, lv->reg, rv->reg);
                    else     i_sub_reg_reg(x, lv->reg, rv->reg);
                    return;
                }
            }
            if (other && leaf_imm(other, &iv)) {
                int64_t delta = add ? iv : -iv;
                bool fits = (lv->ty->kind == TY_BOOL || lv->ty->kind == TY_BYTE)
                            ? imm8_ok(delta) : imm32_ok(delta);
                if (fits) {
                    if (delta == 0) return;
                    if (lv->reg) {
                        if (delta == 1)       i_inc_reg(x, lv->reg);
                        else if (delta == -1) i_dec_reg(x, lv->reg);
                        else i_alu_reg_imm(x, add ? 0 : 5, lv->reg, delta);
                        return;
                    }
                    if (delta == 1)       i_inc_slot(x, lv->global, lv->off, lv->ty);
                    else if (delta == -1) i_dec_slot(x, lv->global, lv->off, lv->ty);
                    else                  i_alu_slot_imm(x, lv->global, lv->off,
                                                         lv->ty, 0, delta);
                    return;
                }
            }
            /* acc = acc + p[i]: fold the element load into the arithmetic
               when the address is a cached base plus a cached/constant
               index. 64-bit elements only; narrower ones would need a
               separate sign-extending load. */
            if (other && lv->reg && !is_ptr_like(lv->ty) &&
                other->kind == NODE_EXPR_INDEX && other->children.count >= 2 &&
                etype(x, other)->size == 8) {
                ASTNode *lb = other->children.items[0];
                ASTNode *li = other->children.items[1];
                Var *bv = lb->kind == NODE_IDENT ? find_var(lb->sval) : NULL;
                Var *iv = li->kind == NODE_IDENT ? find_var(li->sval) : NULL;
                int64_t k = 0;
                if (bv && bv->reg && !bv->is_array) {
                    if (iv && iv->reg && !iv->is_array && (iv->reg & 7) != 4) {
                        i_alu_reg_mem_sib(x, add ? 0 : 1, lv->reg, bv->reg, iv->reg, 8);
                        return;
                    }
                    if (leaf_imm(li, &k) && imm32_ok(k * 8)) {
                        i_alu_reg_mem_disp(x, add ? 0 : 1, lv->reg, bv->reg, (int32_t)(k * 8));
                        return;
                    }
                }
            }
            /* Accumulate an arbitrary expression straight into the cached
               register. Pure right-hand sides only: a side effect could
               change `lv` between the two evaluation points. */
            if (other && lv->reg && !is_ptr_like(lv->ty) && expr_is_pure(other)) {
                gen_expr(x, other);
                if (x->failed) return;
                if (add) i_add_reg_reg(x, lv->reg, 0);
                else     i_sub_reg_reg(x, lv->reg, 0);
                return;
            }
        }
    }

    /* Simple variable target: evaluate the value, store straight to the
       slot (or its register). No address temporary is needed. */
    if (lv && !lv->is_array) {
        if (compound) {
            if (lv->reg) i_mov_reg_reg(x, 0, lv->reg);
            else if (lv->global) i_load_data_reg(x, 0, lv->off, lv->ty);
            else i_load_slot(x, lv->off, lv->ty);
            int scale = 1;
            if (is_ptr_like(lv->ty) &&
                (!strcmp(op, "+") || !strcmp(op, "-")))
                scale = (lv->ty->base && lv->ty->base->size > 0)
                        ? lv->ty->base->size : 1;
            int t = x->temp_depth++;
            gen_binop_from_rax(x, op, lv->ty, rhs, scale, n->line);
            x->temp_depth = t;
            if (x->failed) return;
        } else {
            gen_expr(x, rhs);
            if (x->failed) return;
        }
        if (lv->reg) i_mov_reg_reg(x, lv->reg, 0);
        else if (lv->global) i_store_data_reg(x, lv->off, lv->ty, 0);
        else i_store_slot(x, lv->off, lv->ty);
        return;
    }

    /* arr[i] = value with a register base: evaluate the value first, then
       build the address in rcx. Saves an address temporary on the hot path. */
    if (!compound && !lv && n->lhs->kind == NODE_EXPR_INDEX &&
        n->lhs->children.count >= 2 && expr_is_pure(rhs)) {
        ASTNode *lb = n->lhs->children.items[0];
        ASTNode *li = n->lhs->children.items[1];
        Var *bv = lb->kind == NODE_IDENT ? find_var(lb->sval) : NULL;
        Type *vt = etype(x, n->lhs);
        int esz = vt->size > 0 ? vt->size : 1;
        if (bv && bv->reg && !bv->is_array &&
            (esz == 1 || esz == 2 || esz == 4 || esz == 8)) {
            Var *iv = li->kind == NODE_IDENT ? find_var(li->sval) : NULL;
            int64_t k = 0;
            bool idx_reg = iv && iv->reg && !iv->is_array &&
                           (iv->reg & 7) != 4;
            bool idx_imm = !idx_reg && leaf_imm(li, &k);
            if (idx_reg || idx_imm) {
                gen_expr(x, rhs);
                if (x->failed) return;
                if (idx_reg) {
                    i_lea_rcx_regs(x, bv->reg, iv->reg, esz);
                } else {
                    i_mov_reg_reg(x, 1, bv->reg);
                    if (k != 0) i_alu_reg_imm(x, 0, 1, k * esz);
                }
                i_store_at_rcx(x, vt);
                return;
            }
        }
    }

    Type *t = etype(x, n->lhs);
    int t0 = x->temp_depth++;

    gen_addr(x, n->lhs);
    if (x->failed) { x->temp_depth = t0; return; }
    temp_store(x, t0);

    if (!compound) {
        /* rax keeps the value; only the address needs reloading. */
        gen_expr(x, rhs);
        if (x->failed) { x->temp_depth = t0; return; }
        temp_load_rcx(x, t0);
        x->temp_depth = t0;
        i_store_at_rcx(x, t);
        return;
    }

    int t1 = x->temp_depth++;
    temp_load_rax(x, t0);
    i_load_at_rax(x, t);
    temp_store(x, t1);

    gen_expr(x, rhs);
    if (x->failed) { x->temp_depth = t0; return; }
    int t2 = x->temp_depth++;
    temp_store(x, t2);

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

    temp_load_rcx(x, t0);
    i_store_at_rcx(x, t);
}

static void gen_if(X *x, ASTNode *n) {
    if (n->children.count < 2) { xfail(x, n->line, "malformed if statement"); return; }
    bool has_else = n->children.count > 2 && n->children.items[2];
    if (!has_else) {
        int lend = new_label(x);
        gen_cond_jump(x, n->children.items[0], lend, false);
        if (x->failed) return;
        gen_stmt(x, n->children.items[1]);
        place_label(x, lend);
        return;
    }
    int lelse = new_label(x), lend = new_label(x);
    gen_cond_jump(x, n->children.items[0], lelse, false);
    if (x->failed) return;
    gen_stmt(x, n->children.items[1]);
    i_jmp(x, lend);
    place_label(x, lelse);
    gen_stmt(x, n->children.items[2]);
    place_label(x, lend);
}

static void gen_loop_while(X *x, ASTNode *n) {
    if (n->children.count < 2) { xfail(x, n->line, "malformed while loop"); return; }
    int top = new_label(x), end = new_label(x);
    align_loop(x);
    place_label(x, top);
    gen_cond_jump(x, n->children.items[0], end, false);
    if (x->failed) return;
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
    align_loop(x);
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
    int save_nregmap = g_nregmap;
    int save_nextreg = g_nextreg;

    /* Static register allocation: parameters first, then locals. */
    g_nregmap = 0;
    g_nextreg = 0;
    for (int i = 0; i < np; i++) {
        ASTNode *p = fn->children.items[i];
        Type *pt = fn_param_type(x, p);
        if (scalar_reg_type(pt) && !addr_taken_in(body, p->sval))
            cache_add(p);
    }
    cache_walk(x, body, body);
    int regs_used = g_nextreg;

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
    for (int i = 0; i < regs_used; i++) i_push_reg(x, CACHE_REGS[i]);

    /* parameters */
    for (int i = 0; i < np; i++) {
        ASTNode *p = fn->children.items[i];
        Type *pt = fn_param_type(x, p);
        int bytes = align8(pt->size < 8 ? 8 : pt->size);
        x->locals_used = align8(x->locals_used);
        int off = x->locals_used + bytes;
        x->locals_used = off;
        Var *v = push_var(p->sval, pt, false, 0, off, false);
        int argreg = CALL_REGS[i];
        if (v) v->reg = reg_for_node(p);
        if (v && v->reg) i_mov_reg_reg(x, v->reg, argreg);
        else i_store_slot_reg(x, off, pt, argreg);
    }

    gen_stmt(x, body);

    i_mov_rax_imm(x, 0);
    place_label(x, x->epilogue);
    for (int i = regs_used - 1; i >= 0; i--) i_pop_reg(x, CACHE_REGS[i]);
    e8(x, 0xC9);                                   /* leave */
    e8(x, 0xC3);                                   /* ret */

    g_nscope = save_nscope;
    g_nlocals = save_nlocals;
    g_nregmap = save_nregmap;
    g_nextreg = save_nextreg;
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
    int save_nregmap = g_nregmap;
    int save_nextreg = g_nextreg;

    /* Synthetic code takes no cached registers; clearing the map keeps
       stale entries from a previous function out of this one. */
    g_nregmap = 0;
    g_nextreg = 0;

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
    g_nregmap = save_nregmap;
    g_nextreg = save_nextreg;
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
