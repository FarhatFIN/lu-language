#ifndef LU_H
#define LU_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

/* ─────────────────────────────────────────────
   TOKEN TYPES
   ───────────────────────────────────────────── */
typedef enum {
    /* L1 – SYSTEM */
    TOK_LANG_DECL,       /* Lu/Language          */
    TOK_IMPORT,          /* Import               */
    TOK_MODE,            /* M/ us *mode*         */
    TOK_DEF_CONST,       /* Def:const            */
    TOK_DEF_CONFIG,      /* Def:config           */
    TOK_LANG_BIND,       /* /lang/ us Lu /q[n]   */
    TOK_DEFINE,          /* #define              */
    TOK_INCLUDE,         /* #include             */
    TOK_PRAGMA,          /* #pragma              */
    TOK_IFDEF,           /* #ifdef               */
    TOK_ENDIF,           /* #endif               */

    /* L2 – LOGIC */
    TOK_BLOCK_ID,        /* #q[n]                */
    TOK_BLOCK_END,       /* #q[n]:end            */
    TOK_IF,              /* If/                  */
    TOK_ELIF,            /* Elif/                */
    TOK_ELSE,            /* Else/                */
    TOK_PR,              /* Pr/                  */
    TOK_TO,              /* To/                  */
    TOK_LINREF,          /* Def:anw(...)/Lin     */
    TOK_FUNC,            /* Fn/                  */
    TOK_CALL,            /* Call/                */
    TOK_RETURN,          /* Ret/                 */
    TOK_LOOP,            /* Loop/                */
    TOK_SET,             /* Set/                 */
    TOK_OPT,             /* Opt/                 */
    TOK_ANNOT,           /* @export @async ...   */

    /* L3 – USER */
    TOK_USER,            /* User                 */
    TOK_SND_MES,         /* snd:mes              */
    TOK_REC,             /* Rec/:                */
    TOK_FWD,             /* Fwd:mes              */
    TOK_DEL,             /* Del:mes              */

    /* L4 – NETWORK */
    TOK_SERVER,          /* Server               */
    TOK_COR,             /* cor/                 */
    TOK_INQ,             /* inq;                 */
    TOK_SEND,            /* Snd{}                */
    TOK_ROUTE,           /* Route/               */
    TOK_BCAST,           /* Bcast{}              */

    /* C-LEVEL – MEMORY */
    TOK_PTR,             /* ptr/                 */
    TOK_REF,             /* Ref/                 */
    TOK_DEREF,           /* Deref/               */
    TOK_ALLOC,           /* Alloc/               */
    TOK_FREE,            /* Free/                */
    TOK_MEMSET,          /* Memset/              */
    TOK_MEMCPY,          /* Memcpy/              */

    /* ERROR HANDLING */
    TOK_TRY,             /* Try/                 */
    TOK_CATCH,           /* Catch/               */
    TOK_FINALLY,         /* Finally/             */
    TOK_THROW,           /* Throw/               */

    /* ASYNC */
    TOK_AWAIT,           /* Await/               */
    TOK_SPAWN,           /* Spawn/               */
    TOK_CHAN,            /* Chan/                */
    TOK_CHAN_SEND,       /* Send/inbox ←         */
    TOK_CHAN_RECV,       /* Recv/                */
    TOK_SELECT,          /* Select/              */
    TOK_CASE,            /* Case/                */
    TOK_DEFAULT,         /* Default/             */
    TOK_EVENT,           /* Event/               */
    TOK_ON,              /* On/                  */
    TOK_EMIT,            /* Emit/                */
    TOK_OFF,             /* Off/                 */

    /* DEBUG */
    TOK_LOG,             /* Log/                 */
    TOK_ASSERT,          /* Assert/              */
    TOK_TRACE,           /* Trace/               */
    TOK_BREAK_BP,        /* Break/               */
    TOK_WATCH,           /* Watch/               */

    /* COMPOSITE TYPES */
    TOK_ENUM,
    TOK_STRUCT,
    TOK_UNION,
    TOK_TUPLE,
    TOK_MODULE,
    TOK_NS,
    TOK_USE_NS,

    /* TYPES */
    TOK_TYPE_INT,
    TOK_TYPE_INT64,
    TOK_TYPE_FLOAT,
    TOK_TYPE_STR,
    TOK_TYPE_BOOL,
    TOK_TYPE_BYTE,
    TOK_TYPE_IP,
    TOK_TYPE_ID,
    TOK_TYPE_COR,
    TOK_TYPE_MSG,
    TOK_TYPE_LIB,
    TOK_TYPE_VOID,

    /* GAME ENGINE — игровые команды */
    TOK_CR,            /* cr(obj;name)    — создать объект        */
    TOK_DAM,           /* dam(obj;name)   — нанести урон          */
    TOK_ONL,           /* onl(obj;name)   — только для объекта    */
    TOK_VL,            /* vl              — переменная значения   */
    TOK_OBJ,           /* obj             — объект                */
    TOK_FUNC_CALL,     /* func(name)      — вызов функции объекта */
    TOK_EXP,           /* exp             — создать/spawn         */
    TOK_CL,            /* cl              — вызов блока           */
    TOK_DIST,          /* distance        — дистанция между obj   */

    /* LITERALS & IDENTIFIERS */
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STR_LIT,
    TOK_BOOL_LIT,
    TOK_IP_LIT,
    TOK_IDENT,

    /* OPERATORS */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_DSTAR,      /* ** */
    TOK_INC,        /* ++ */
    TOK_DEC,        /* -- */
    TOK_AMP,        /* &  */
    TOK_PIPE,       /* |  */
    TOK_CARET,      /* ^  */
    TOK_TILDE,      /* ~  */
    TOK_LSHIFT,     /* << */
    TOK_RSHIFT,     /* >> */
    TOK_URSHIFT,    /* >>> */
    TOK_AND,        /* && */
    TOK_OR,         /* || */
    TOK_BANG,       /* !  */
    TOK_EQ,         /* == */
    TOK_NEQ,        /* != */
    TOK_LT,         /* <  */
    TOK_GT,         /* >  */
    TOK_LE,         /* <= */
    TOK_GE,         /* >= */
    TOK_QUESTION,   /* ?  */
    TOK_COLON,      /* :  */
    TOK_ARROW,      /* →  */
    TOK_LARROW,     /* ←  */
    TOK_ASSIGN,     /* =  */
    TOK_PLUS_EQ,    /* += */
    TOK_MINUS_EQ,   /* -= */
    TOK_STAR_EQ,    /* *= */
    TOK_SLASH_EQ,   /* /= */
    TOK_PERCENT_EQ, /* %= */
    TOK_AMP_EQ,     /* &= */
    TOK_PIPE_EQ,    /* |= */
    TOK_CARET_EQ,   /* ^= */
    TOK_LSHIFT_EQ,  /* <<= */
    TOK_RSHIFT_EQ,  /* >>= */
    TOK_DOT,        /* .  */
    TOK_PTR_ACCESS, /* -> */
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,

    TOK_NEWLINE,
    TOK_EOF,
    TOK_UNKNOWN
} TokenType;

/* ─────────────────────────────────────────────
   TOKEN
   ───────────────────────────────────────────── */
typedef struct {
    TokenType type;
    char     *value;   /* heap-allocated copy */
    int       line;
    int       col;
} Token;

/* ─────────────────────────────────────────────
   AST NODE KINDS
   ───────────────────────────────────────────── */
typedef enum {
    NODE_PROGRAM,
    NODE_LANG_DECL,
    NODE_IMPORT,
    NODE_MODE,
    NODE_DEF_CONST,
    NODE_DEF_CONFIG,
    NODE_DEFINE,
    NODE_INCLUDE,
    NODE_BLOCK,         /* #q[n] */
    NODE_IF,
    NODE_LOOP,
    NODE_LOOP_WHILE,
    NODE_LOOP_EACH,
    NODE_PR,
    NODE_TO,
    NODE_SET,
    NODE_VAR_DECL,
    NODE_FUNC_DECL,
    NODE_FUNC_CALL,
    NODE_RETURN,
    NODE_LINREF,
    NODE_USER_DECL,
    NODE_SERVER_DECL,
    NODE_SEND,
    NODE_RECV,
    NODE_BCAST,
    NODE_ROUTE,
    NODE_INQ,
    NODE_PTR_DECL,
    NODE_ALLOC,
    NODE_FREE,
    NODE_MEMSET,
    NODE_MEMCPY,
    NODE_TRY,
    NODE_THROW,
    NODE_STRUCT_DECL,
    NODE_ENUM_DECL,
    NODE_UNION_DECL,
    NODE_TUPLE_DECL,
    NODE_NAMESPACE,
    NODE_MODULE,
    NODE_ASYNC_FUNC,
    NODE_AWAIT,
    NODE_SPAWN,
    NODE_CHAN_DECL,
    NODE_CHAN_SEND,
    NODE_CHAN_RECV,
    NODE_SELECT,
    NODE_EVENT,
    NODE_ON,
    NODE_EMIT,
    NODE_LOG,
    NODE_ASSERT,
    NODE_ANNOT,
    NODE_OPT,
    NODE_EXPR_BINOP,
    NODE_EXPR_UNOP,
    NODE_EXPR_TERNARY,
    NODE_EXPR_INDEX,
    NODE_EXPR_FIELD,
    NODE_EXPR_DEREF,
    NODE_EXPR_REF,
    NODE_LITERAL_INT,
    NODE_LITERAL_FLOAT,
    NODE_LITERAL_STR,
    NODE_LITERAL_BOOL,
    NODE_LITERAL_IP,
    NODE_LITERAL_COR,
    NODE_IDENT,
    /* GAME ENGINE узлы */
    NODE_CR,
    NODE_DAM,
    NODE_ONL,
    NODE_FUNC_CALL_GAME,
    NODE_EXP,
    NODE_CL,

    /* C++-inspired extensions lowered to C */
    NODE_CLASS_DECL = 250,
    NODE_CLASS_BODY,
    NODE_METHOD_DECL,
    NODE_INHERIT,
    NODE_NEW_EXPR,
    NODE_DELETE_EXPR,
    NODE_TEMPLATE_DECL,
    NODE_TEMPLATE_INST,
    NODE_OVERRIDE_DECL,
    NODE_VIRTUAL_DECL,
    NODE_OP_OVERLOAD,
    NODE_DESTRUCTOR,
    NODE_CONSTRUCTOR,
    NODE_INTERFACE_DECL,
    NODE_IMPL_DECL,
    NODE_THIS_EXPR,
    NODE_SUPER_EXPR,
    NODE_OBJ_DECL,
    NODE_CL_CALL,
    NODE_DAM_CALL,
    NODE_ONL_CALL,
    NODE_SND_GOTO,
    /* v2.0 additions */
    NODE_FSTRING,       /* f-string: list of parts (literal str or expr) */
    NODE_MATCH,         /* match/case */
    NODE_DEFER          /* defer statement */
} NodeKind;

/* ─────────────────────────────────────────────
   AST NODE
   ───────────────────────────────────────────── */
struct ASTNode;
typedef struct ASTNode ASTNode;

typedef struct {
    ASTNode **items;
    int       count;
    int       cap;
} NodeList;

struct ASTNode {
    NodeKind  kind;
    int       line;

    /* Generic children */
    NodeList  children;

    /* Common payload fields */
    char     *sval;    /* string value / name        */
    int64_t   ival;    /* integer value              */
    double    fval;    /* float value                */
    bool      bval;    /* bool value                 */
    int       block_n; /* for #q[n]                  */
    char     *type_name;  /* for typed declarations  */
    char     *op;         /* operator symbol          */
    char     *annot;      /* annotation name / f-string part kind: "lit" or "expr" */
};

/* ─────────────────────────────────────────────
   LEXER STATE
   ───────────────────────────────────────────── */
typedef struct {
    const char *src;
    int         pos;
    int         line;
    int         col;
    int         len;
} Lexer;

/* ─────────────────────────────────────────────
   PARSER STATE
   ───────────────────────────────────────────── */
typedef struct {
    Token  *tokens;
    int     count;
    int     pos;
} Parser;

/* ─────────────────────────────────────────────
   SYMBOL TABLE
   ───────────────────────────────────────────── */
#define SYM_MAX 4096

typedef struct {
    char  name[128];
    char  type[32];
    int   block_scope; /* -1 = global */
    bool  is_ptr;
    bool  is_func;
    bool  is_const;
    int   decl_line;   /* line where symbol was declared */
} Symbol;

typedef struct {
    Symbol entries[SYM_MAX];
    int    count;
} SymTable;

/* ─────────────────────────────────────────────
   BLOCK REGISTRY  (for Lin resolution)
   ───────────────────────────────────────────── */
#define BLOCK_REG_MAX 512

typedef struct {
    int   block_n;
    char  pr_expr[256];   /* first Pr/ expression text in block */
    char  to_answer[256]; /* first To/ response text in block   */
    bool  has_pr;
    bool  has_to;
} BlockInfo;

typedef struct {
    BlockInfo entries[BLOCK_REG_MAX];
    int       count;
} BlockRegistry;

/* ─────────────────────────────────────────────
   CODEGEN STATE
   ───────────────────────────────────────────── */

/* Class registry entry: maps class name → its AST node, so codegen
   can resolve inherited methods. */
#define CLASS_REG_MAX 256
typedef struct {
    char     name[128];
    ASTNode *node;   /* the NODE_CLASS_DECL */
} ClassEntry;

typedef struct {
    FILE    *out;
    int      indent;
    int      opt_level;        /* 0-3 from Opt/O[n] */
    bool     debug_mode;
    SymTable syms;
    BlockRegistry breg;
    int      tmp_counter;      /* for temp var names */
    int      label_counter;    /* for goto labels    */
    /* block id list (registered after generation) */
    int      blocks[4096];
    int      block_count;
    /* current block scope (-1 = global) */
    int      cur_block;
    /* class registry for inheritance resolution */
    ClassEntry classes[CLASS_REG_MAX];
    int       class_count;
    /* current class being generated (for super.method() resolution) */
    ASTNode *current_class;
} Codegen;

/* ─────────────────────────────────────────────
   FUNCTION PROTOTYPES
   ───────────────────────────────────────────── */

/* lexer.c */
Lexer  *lexer_new(const char *src);
void    lexer_free(Lexer *l);
Token  *lexer_tokenize(Lexer *l, int *out_count);
void    tokens_free(Token *toks, int count);
const char *token_type_name(TokenType t);

/* parser.c */
ASTNode *parse(Token *tokens, int count);
void     ast_free(ASTNode *node);
void     ast_print(ASTNode *node, int depth);

/* semantic.c */
bool  semantic_check(ASTNode *root, SymTable *syms);

/* codegen.c */
void  codegen_run(ASTNode *root, FILE *out, int opt_level, bool debug);

/* util.c */
char  *lu_strdup(const char *s);
void   node_list_add(NodeList *l, ASTNode *n);
ASTNode *node_new(NodeKind kind, int line);
void   lu_error(int line, const char *fmt, ...);
void   lu_warn(int line, const char *fmt, ...);
extern int g_parse_error_count;  /* incremented on parse errors; main() checks this */

/* block_registry helpers (used by semantic + codegen) */
BlockInfo *breg_find(BlockRegistry *br, int block_n);
BlockInfo *breg_get_or_create(BlockRegistry *br, int block_n);

#endif /* LU_H */
