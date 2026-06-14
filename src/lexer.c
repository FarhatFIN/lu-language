#include "lu.h"
#include <stdarg.h>

/* ─────────────────────────────────────────────
   helpers
   ───────────────────────────────────────────── */
static char cur(Lexer *l) {
    return (l->pos < l->len) ? l->src[l->pos] : '\0';
}
static char peek(Lexer *l, int off) {
    int p = l->pos + off;
    return (p < l->len) ? l->src[p] : '\0';
}
static void advance(Lexer *l) {
    if (l->pos < l->len) {
        if (l->src[l->pos] == '\n') { l->line++; l->col = 1; }
        else l->col++;
        l->pos++;
    }
}
static bool starts_with(Lexer *l, const char *s) {
    int len = strlen(s);
    if (l->pos + len > l->len) return false;
    return strncmp(l->src + l->pos, s, len) == 0;
}
static void skip_spaces(Lexer *l) {
    while (cur(l) == ' ' || cur(l) == '\t' || cur(l) == '\r') advance(l);
}
static void skip_line(Lexer *l) {
    while (cur(l) && cur(l) != '\n') advance(l);
}

/* Token list helpers */
typedef struct { Token *data; int count; int cap; } TokList;
static void tl_add(TokList *tl, Token t) {
    if (tl->count == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 256;
        Token *grown = realloc(tl->data, sizeof(Token) * tl->cap);
        if (!grown) { fprintf(stderr, "out of memory\n"); exit(1); }
        tl->data = grown;
    }
    tl->data[tl->count++] = t;
}
static Token make_tok(TokenType type, const char *val, int line, int col) {
    Token t; t.type = type; t.value = lu_strdup(val); t.line = line; t.col = col;
    return t;
}

/* ─────────────────────────────────────────────
   read helpers
   ───────────────────────────────────────────── */

/* read identifier-like word (letters, digits, underscore, dot) */
static char *read_word(Lexer *l) {
    int start = l->pos;
    while (cur(l) && (isalnum((unsigned char)cur(l)) || cur(l) == '_' || cur(l) == '.'))
        advance(l);
    int len = l->pos - start;
    char *w = malloc(len + 1);
    if (!w) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(w, l->src + start, len);
    w[len] = '\0';
    return w;
}

/* read quoted string «...» or "..." or """...""" */
static char *read_string(Lexer *l) {
    char buf[4096];
    int bi = 0;
    bool triple = false;
    bool guillemet = false;

    /* Opening delimiter. The caller must leave the lexer positioned at it. */
    if (cur(l) == '"' && peek(l, 1) == '"' && peek(l, 2) == '"') {
        triple = true;
        advance(l); advance(l); advance(l);
    } else if ((unsigned char)cur(l) == 0xC2 && (unsigned char)peek(l, 1) == 0xAB) {
        guillemet = true; /* « */
        advance(l); advance(l);
    } else if (cur(l) == '"') {
        advance(l);
    }

    while (cur(l)) {
        if (triple && cur(l) == '"' && peek(l, 1) == '"' && peek(l, 2) == '"') {
            advance(l); advance(l); advance(l);
            break;
        }
        if (guillemet && (unsigned char)cur(l) == 0xC2 && (unsigned char)peek(l, 1) == 0xBB) {
            advance(l); advance(l); /* » */
            break;
        }
        if (!triple && !guillemet && cur(l) == '"') {
            advance(l);
            break;
        }

        if (cur(l) == '\\') {
            advance(l);
            char e = cur(l);
            if (e) advance(l);
            switch (e) {
                case 'n':  buf[bi++] = '\n'; break;
                case 't':  buf[bi++] = '\t'; break;
                case 'r':  buf[bi++] = '\r'; break;
                case '\\': buf[bi++] = '\\'; break;
                case '"':  buf[bi++] = '"'; break;
                case '0':  buf[bi++] = '\0'; break;
                default:
                    buf[bi++] = '\\';
                    if (e) buf[bi++] = e;
                    break;
            }
        } else {
            buf[bi++] = cur(l);
            advance(l);
        }
        if (bi >= (int)sizeof(buf) - 8) break;
    }
    buf[bi] = '\0';
    return lu_strdup(buf);
}

/* read number: decimal, hex 0x, binary 0b, octal 0o, floats with exponent and f suffix */
static Token read_number(Lexer *l) {
    int start = l->pos; int line = l->line; int col = l->col;
    bool is_float = false;

    if (cur(l) == '0' && (peek(l, 1) == 'x' || peek(l, 1) == 'X')) {
        advance(l); advance(l);
        while (cur(l) && isxdigit((unsigned char)cur(l))) advance(l);
    } else if (cur(l) == '0' && (peek(l, 1) == 'b' || peek(l, 1) == 'B')) {
        advance(l); advance(l);
        while (cur(l) == '0' || cur(l) == '1') advance(l);
    } else if (cur(l) == '0' && (peek(l, 1) == 'o' || peek(l, 1) == 'O')) {
        advance(l); advance(l);
        while (cur(l) >= '0' && cur(l) <= '7') advance(l);
    } else {
        bool seen_dot = false;
        while (cur(l) && (isdigit((unsigned char)cur(l)) || cur(l) == '.')) {
            if (cur(l) == '.') {
                /* a second dot (or dot not followed by a digit) ends the literal,
                   so 1.2.3 never lexes as a single number */
                if (seen_dot || !isdigit((unsigned char)peek(l, 1))) break;
                seen_dot = true; is_float = true;
            }
            advance(l);
        }
        /* exponent: 1e9, 1.5e-3, 2E+7 */
        if ((cur(l) == 'e' || cur(l) == 'E') &&
            (isdigit((unsigned char)peek(l, 1)) ||
             ((peek(l, 1) == '+' || peek(l, 1) == '-') && isdigit((unsigned char)peek(l, 2))))) {
            is_float = true;
            advance(l); /* e/E */
            if (cur(l) == '+' || cur(l) == '-') advance(l);
            while (cur(l) && isdigit((unsigned char)cur(l))) advance(l);
        }
    }

    int len = l->pos - start;
    char *s = malloc(len + 1);
    if (!s) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(s, l->src + start, len); s[len] = '\0';
    /* optional float suffix 3.14f — consumed, not stored */
    if (is_float && (cur(l) == 'f' || cur(l) == 'F')) advance(l);
    Token t; t.value = s; t.line = line; t.col = col;
    t.type = is_float ? TOK_FLOAT_LIT : TOK_INT_LIT;
    return t;
}

/* read cor key {26,16,...} */
static char *read_cor(Lexer *l) {
    char buf[512]; int bi = 0;
    advance(l); /* skip { */
    while (cur(l) && cur(l) != '}') {
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = cur(l);
        advance(l);
    }
    if (cur(l) == '}') advance(l);
    buf[bi] = '\0';
    return lu_strdup(buf);
}

/* read Snd{...} / Bcast{...} payload */
static char *read_brace_payload(Lexer *l) {
    char buf[512]; int bi = 0;
    advance(l); /* skip { */
    while (cur(l) && cur(l) != '}') {
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = cur(l);
        advance(l);
    }
    if (cur(l) == '}') advance(l);
    buf[bi] = '\0';
    return lu_strdup(buf);
}

/* read [msg] payload */
static char *read_bracket_payload(Lexer *l) {
    char buf[256]; int bi = 0;
    advance(l); /* skip [ */
    while (cur(l) && cur(l) != ']') {
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = cur(l);
        advance(l);
    }
    if (cur(l) == ']') advance(l);
    buf[bi] = '\0';
    return lu_strdup(buf);
}

/* ─────────────────────────────────────────────
   keyword → token type
   ───────────────────────────────────────────── */
static TokenType keyword_type(const char *w) {
    if (!strcmp(w, "Import"))    return TOK_IMPORT;
    if (!strcmp(w, "User"))      return TOK_USER;
    if (!strcmp(w, "Server"))    return TOK_SERVER;
    if (!strcmp(w, "Module"))    return TOK_MODULE;
    if (!strcmp(w, "Ns"))        return TOK_NS;
    if (!strcmp(w, "Use"))       return TOK_USE_NS;
    if (!strcmp(w, "enum"))      return TOK_ENUM;
    if (!strcmp(w, "struct"))    return TOK_STRUCT;
    if (!strcmp(w, "union"))     return TOK_UNION;
    if (!strcmp(w, "tuple"))     return TOK_TUPLE;
    if (!strcmp(w, "int"))       return TOK_TYPE_INT;
    if (!strcmp(w, "int64"))     return TOK_TYPE_INT64;
    if (!strcmp(w, "float"))     return TOK_TYPE_FLOAT;
    if (!strcmp(w, "str"))       return TOK_TYPE_STR;
    if (!strcmp(w, "bool"))      return TOK_TYPE_BOOL;
    if (!strcmp(w, "byte"))      return TOK_TYPE_BYTE;
    if (!strcmp(w, "ip"))        return TOK_TYPE_IP;
    if (!strcmp(w, "id"))        return TOK_TYPE_ID;
    if (!strcmp(w, "cor"))       return TOK_TYPE_COR;
    if (!strcmp(w, "msg"))       return TOK_TYPE_MSG;
    if (!strcmp(w, "lib"))       return TOK_TYPE_LIB;
    if (!strcmp(w, "void"))      return TOK_TYPE_VOID;
    if (!strcmp(w, "true") || !strcmp(w, "false")) return TOK_BOOL_LIT;
    return TOK_IDENT;
}

/* ─────────────────────────────────────────────
   MAIN TOKENIZER
   ───────────────────────────────────────────── */
Lexer *lexer_new(const char *src) {
    Lexer *l = calloc(1, sizeof(Lexer));
    l->src = src; l->len = strlen(src); l->line = 1; l->col = 1;
    return l;
}
void lexer_free(Lexer *l) { free(l); }

Token *lexer_tokenize(Lexer *l, int *out_count) {
    TokList tl = {0};
    while (l->pos < l->len) {
        skip_spaces(l);
        if (l->pos >= l->len) break;

        int line = l->line, col = l->col;
        char c = cur(l);

        /* Comments: line and block comments */
        if (c == '/' && peek(l, 1) == '/') { skip_line(l); continue; }
        if (c == '/' && peek(l, 1) == '*') {
            advance(l); advance(l);
            while (cur(l) && !(cur(l) == '*' && peek(l, 1) == '/'))
                advance(l);
            if (cur(l) == '*' && peek(l, 1) == '/') { advance(l); advance(l); }
            continue;
        }

        /* ── Newline ── */
        if (c == '\n') { advance(l); tl_add(&tl, make_tok(TOK_NEWLINE, "\n", line, col)); continue; }

        /* ── Unicode guillemet string « (UTF-8: 0xC2 0xAB) ── */
        if ((unsigned char)c == 0xC2 && (unsigned char)peek(l, 1) == 0xAB) {
            char *s = read_string(l);
            tl_add(&tl, make_tok(TOK_STR_LIT, s, line, col)); free(s); continue;
        }

        /* ── Triple or regular string ── */
        if (c == '"') {
            char *s = read_string(l);
            tl_add(&tl, make_tok(TOK_STR_LIT, s, line, col)); free(s); continue;
        }

        /* ── Template string $« ── */
        if (c == '$') {
            advance(l);
            if ((unsigned char)cur(l) == 0xC2 && (unsigned char)peek(l, 1) == 0xAB) {
                char *s = read_string(l);
                /* mark as template by prefixing $ */
                char buf[4096]; snprintf(buf, sizeof(buf), "$%s", s);
                tl_add(&tl, make_tok(TOK_STR_LIT, buf, line, col)); free(s); continue;
            }
        }

        /* ── Numbers ── */
        if (isdigit((unsigned char)c)) {
            Token t = read_number(l); t.line = line; t.col = col;
            tl_add(&tl, t); continue;
        }

        /* ── Preprocessor directives #define #include #q[n] ── */
        if (c == '#') {
            advance(l);
            if (isdigit((unsigned char)cur(l))) {
                /* error: # must be followed by q */
                tl_add(&tl, make_tok(TOK_UNKNOWN, "#", line, col)); continue;
            }
            char *w = read_word(l);
            if (!strcmp(w, "define")) {
                tl_add(&tl, make_tok(TOK_DEFINE, "#define", line, col));
            } else if (!strcmp(w, "include")) {
                tl_add(&tl, make_tok(TOK_INCLUDE, "#include", line, col));
            } else if (!strcmp(w, "pragma")) {
                tl_add(&tl, make_tok(TOK_PRAGMA, "#pragma", line, col));
            } else if (!strcmp(w, "ifdef")) {
                tl_add(&tl, make_tok(TOK_IFDEF, "#ifdef", line, col));
            } else if (!strcmp(w, "endif")) {
                tl_add(&tl, make_tok(TOK_ENDIF, "#endif", line, col));
            } else if (w[0] == 'q') {
                /* block id: q1, q2 ... */
                int n = atoi(w + 1);
                char nbuf[32]; snprintf(nbuf, sizeof(nbuf), "%d", n);
                /* check :end BEFORE emitting BLOCK_ID */
                if (cur(l) == ':') {
                    advance(l);
                    char *end = read_word(l);
                    if (!strcmp(end, "end"))
                        /* #q[n]:end  → emit only BLOCK_END, not BLOCK_ID */
                        tl_add(&tl, make_tok(TOK_BLOCK_END, nbuf, line, col));
                    else
                        /* #q[n]:something_else → emit BLOCK_ID, ignore suffix */
                        tl_add(&tl, make_tok(TOK_BLOCK_ID, nbuf, line, col));
                    free(end);
                } else {
                    /* plain #q[n] → emit BLOCK_ID */
                    tl_add(&tl, make_tok(TOK_BLOCK_ID, nbuf, line, col));
                }
            } else {
                char buf[128]; snprintf(buf, sizeof(buf), "#%s", w);
                tl_add(&tl, make_tok(TOK_IDENT, buf, line, col));
            }
            free(w); continue;
        }

        /* ── @ annotations ── */
        if (c == '@') {
            advance(l);
            char *w = read_word(l);
            tl_add(&tl, make_tok(TOK_ANNOT, w, line, col)); free(w); continue;
        }

        /* ── Multi-char operators before single-char ── */
        if (c == '>' && peek(l, 1) == '>' && peek(l, 2) == '>') {
            advance(l); advance(l); advance(l);
            tl_add(&tl, make_tok(TOK_URSHIFT, ">>>", line, col)); continue;
        }
        if (c == '>' && peek(l, 1) == '>') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_RSHIFT, ">>", line, col)); continue;
        }
        if (c == '<' && peek(l, 1) == '-') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_LARROW, "<-", line, col)); continue;
        }
        if (c == '<' && peek(l, 1) == '<') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_LSHIFT, "<<", line, col)); continue;
        }
        if (c == '*' && peek(l, 1) == '*') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_DSTAR, "**", line, col)); continue;
        }
        if (c == '+' && peek(l, 1) == '+') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_INC, "++", line, col)); continue;
        }
        if (c == '-' && peek(l, 1) == '-') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_DEC, "--", line, col)); continue;
        }
        if (c == '&' && peek(l, 1) == '&') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_AND, "&&", line, col)); continue;
        }
        if (c == '|' && peek(l, 1) == '|') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_OR, "||", line, col)); continue;
        }
        if (c == '=' && peek(l, 1) == '=') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_EQ, "==", line, col)); continue;
        }
        if (c == '!' && peek(l, 1) == '=') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_NEQ, "!=", line, col)); continue;
        }
        if (c == '<' && peek(l, 1) == '=') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_LE, "<=", line, col)); continue;
        }
        if (c == '>' && peek(l, 1) == '=') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_GE, ">=", line, col)); continue;
        }
        if (c == '-' && peek(l, 1) == '>') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_PTR_ACCESS, "->", line, col)); continue;
        }
        /* UTF-8 arrow → (0xE2 0x86 0x92) */
        if ((unsigned char)c == 0xE2 && (unsigned char)peek(l, 1) == 0x86 && (unsigned char)peek(l, 2) == 0x92) {
            advance(l); advance(l); advance(l); tl_add(&tl, make_tok(TOK_ARROW, "->", line, col)); continue;
        }
        /* UTF-8 arrow ← (0xE2 0x86 0x90) */
        if ((unsigned char)c == 0xE2 && (unsigned char)peek(l, 1) == 0x86 && (unsigned char)peek(l, 2) == 0x90) {
            advance(l); advance(l); advance(l); tl_add(&tl, make_tok(TOK_LARROW, "<-", line, col)); continue;
        }

        /* ── Lu/ prefix keywords ── */
        if (c == 'L' && starts_with(l, "Lu/Language")) {
            for (int i = 0; i < 11; i++) advance(l);
            tl_add(&tl, make_tok(TOK_LANG_DECL, "Lu/Language", line, col)); continue;
        }

        /* ── If/ Elif/ Else/ Pr/ To/ Fn/ Ret/ Call/ Loop/ Set/ Opt/ ── */
#define KW_SLASH(kw, tok) \
        if (starts_with(l, kw "/")) { \
            for (int i = 0; kw[i]; i++) { advance(l); } advance(l); \
            tl_add(&tl, make_tok(tok, kw "/", line, col)); continue; \
        }
        KW_SLASH("If",     TOK_IF)
        KW_SLASH("Elif",   TOK_ELIF)
        KW_SLASH("Else",   TOK_ELSE)
        KW_SLASH("Pr",     TOK_PR)
        KW_SLASH("To",     TOK_TO)
        KW_SLASH("Ret",    TOK_RETURN)
        KW_SLASH("Loop",   TOK_LOOP)
        KW_SLASH("Set",    TOK_SET)
        KW_SLASH("Opt",    TOK_OPT)
        KW_SLASH("Ref",    TOK_REF)
        KW_SLASH("Deref",  TOK_DEREF)
        KW_SLASH("Alloc",  TOK_ALLOC)
        KW_SLASH("Free",   TOK_FREE)
        KW_SLASH("Memset", TOK_MEMSET)
        KW_SLASH("Memcpy", TOK_MEMCPY)
        KW_SLASH("Try",    TOK_TRY)
        KW_SLASH("Catch",  TOK_CATCH)
        KW_SLASH("Finally",TOK_FINALLY)
        KW_SLASH("Throw",  TOK_THROW)
        KW_SLASH("Await",  TOK_AWAIT)
        KW_SLASH("Spawn",  TOK_SPAWN)
        KW_SLASH("Chan",   TOK_CHAN)
        KW_SLASH("Select", TOK_SELECT)
        KW_SLASH("Case",   TOK_CASE)
        KW_SLASH("Default",TOK_DEFAULT)
        KW_SLASH("Event",  TOK_EVENT)
        KW_SLASH("On",     TOK_ON)
        KW_SLASH("Emit",   TOK_EMIT)
        KW_SLASH("Off",    TOK_OFF)
        KW_SLASH("Log",    TOK_LOG)
        KW_SLASH("Assert", TOK_ASSERT)
        KW_SLASH("Trace",  TOK_TRACE)
        KW_SLASH("Break",  TOK_BREAK_BP)
        KW_SLASH("Watch",  TOK_WATCH)
        KW_SLASH("Route",  TOK_ROUTE)
        KW_SLASH("Send",   TOK_CHAN_SEND)
        KW_SLASH("Recv",   TOK_CHAN_RECV)
        KW_SLASH("Fwd",    TOK_FWD)   /* Fwd:mes handled by parser */
        KW_SLASH("Del",    TOK_DEL)
#undef KW_SLASH

        /* Fn/ with possible name prefix */
        if (starts_with(l, "Fn/")) {
            advance(l); advance(l); advance(l);
            tl_add(&tl, make_tok(TOK_FUNC, "Fn/", line, col)); continue;
        }
        if (starts_with(l, "Call/")) {
            advance(l); advance(l); advance(l); advance(l); advance(l);
            tl_add(&tl, make_tok(TOK_CALL, "Call/", line, col)); continue;
        }

        /* ── GAME ENGINE команды ── */
        /* cr(obj;name) — создать объект */
        if (starts_with(l, "cr(")) {
            advance(l); advance(l); advance(l); /* skip cr( */
            char buf[128]; int bi = 0;
            while (cur(l) && cur(l) != ')') {
                if (bi < (int)sizeof(buf) - 1) buf[bi++] = cur(l);
                advance(l);
            }
            buf[bi] = '\0';
            if (cur(l) == ')') advance(l);
            tl_add(&tl, make_tok(TOK_CR, buf, line, col)); continue;
        }
        /* dam(obj;name) — нанести урон */
        if (starts_with(l, "dam(")) {
            advance(l); advance(l); advance(l); advance(l); /* skip dam( */
            char buf[128]; int bi = 0;
            while (cur(l) && cur(l) != ')') {
                if (bi < (int)sizeof(buf) - 1) buf[bi++] = cur(l);
                advance(l);
            }
            buf[bi] = '\0';
            if (cur(l) == ')') advance(l);
            tl_add(&tl, make_tok(TOK_DAM, buf, line, col)); continue;
        }
        /* onl(obj;name) — только для этого объекта */
        if (starts_with(l, "onl(")) {
            advance(l); advance(l); advance(l); advance(l); /* skip onl( */
            char buf[128]; int bi = 0;
            while (cur(l) && cur(l) != ')') {
                if (bi < (int)sizeof(buf) - 1) buf[bi++] = cur(l);
                advance(l);
            }
            buf[bi] = '\0';
            if (cur(l) == ')') advance(l);
            tl_add(&tl, make_tok(TOK_ONL, buf, line, col)); continue;
        }
        /* func(name) — вызов метода объекта */
        if (starts_with(l, "func(")) {
            advance(l); advance(l); advance(l); advance(l); advance(l); /* skip func( */
            char buf[128]; int bi = 0;
            while (cur(l) && cur(l) != ')') {
                if (bi < (int)sizeof(buf) - 1) buf[bi++] = cur(l);
                advance(l);
            }
            buf[bi] = '\0';
            if (cur(l) == ')') advance(l);
            tl_add(&tl, make_tok(TOK_FUNC_CALL, buf, line, col)); continue;
        }
        /* exp/cl are parsed as normal identifiers. This lets code use names
           such as `int exp` without the lexer turning them into commands. */
        /* distance — дистанция (только как отдельное слово) */
        if (starts_with(l, "distance") &&
            !(isalnum((unsigned char)peek(l, 8)) || peek(l, 8) == '_' || peek(l, 8) == '.')) {
            for (int i=0;i<8;i++) advance(l);
            tl_add(&tl, make_tok(TOK_DIST, "distance", line, col)); continue;
        }

        /* M/ */
        if (c == 'M' && peek(l, 1) == '/') {
            advance(l); advance(l); tl_add(&tl, make_tok(TOK_MODE, "M/", line, col)); continue;
        }

        /* ptr/ */
        if (starts_with(l, "ptr/")) {
            advance(l); advance(l); advance(l); advance(l);
            tl_add(&tl, make_tok(TOK_PTR, "ptr/", line, col)); continue;
        }

        /* cor/ cor:{...} — check for type then key */
        if (starts_with(l, "cor/")) {
            advance(l); advance(l); advance(l); advance(l);
            if (cur(l) == '{') {
                char *k = read_cor(l);
                tl_add(&tl, make_tok(TOK_COR, k, line, col)); free(k);
            } else {
                tl_add(&tl, make_tok(TOK_TYPE_COR, "cor", line, col));
            }
            continue;
        }

        /* inq; */
        if (starts_with(l, "inq;")) {
            advance(l); advance(l); advance(l); advance(l);
            tl_add(&tl, make_tok(TOK_INQ, "inq;", line, col)); continue;
        }

        /* snd:mes */
        if (starts_with(l, "snd:mes")) {
            for (int i = 0; i < 7; i++) advance(l);
            tl_add(&tl, make_tok(TOK_SND_MES, "snd:mes", line, col)); continue;
        }

        /* Rec/: */
        if (starts_with(l, "Rec/:")) {
            advance(l); advance(l); advance(l); advance(l); advance(l);
            tl_add(&tl, make_tok(TOK_REC, "Rec/:", line, col)); continue;
        }

        /* Snd{ */
        if (starts_with(l, "Snd{")) {
            advance(l); advance(l); advance(l);
            char *p = read_brace_payload(l);
            tl_add(&tl, make_tok(TOK_SEND, p, line, col)); free(p); continue;
        }

        /* Bcast{ */
        if (starts_with(l, "Bcast{")) {
            for (int i = 0; i < 5; i++) advance(l);
            char *p = read_brace_payload(l);
            tl_add(&tl, make_tok(TOK_BCAST, p, line, col)); free(p); continue;
        }

        /* Def: */
        if (starts_with(l, "Def:const")) {
            for (int i = 0; i < 9; i++) advance(l);
            tl_add(&tl, make_tok(TOK_DEF_CONST, "Def:const", line, col)); continue;
        }
        if (starts_with(l, "Def:config")) {
            for (int i = 0; i < 10; i++) advance(l);
            tl_add(&tl, make_tok(TOK_DEF_CONFIG, "Def:config", line, col)); continue;
        }
        if (starts_with(l, "Def:anw(")) {
            /* read until /Lin */
            for (int i = 0; i < 8; i++) advance(l); /* skip "Def:anw(" */
            char buf[256]; int bi = 0;
            while (cur(l) && cur(l) != ')') {
                if (bi < (int)sizeof(buf) - 1) buf[bi++] = cur(l);
                advance(l);
            }
            buf[bi] = '\0';
            if (cur(l) == ')') advance(l);
            /* expect /Lin */
            if (starts_with(l, "/Lin")) { for (int i = 0; i < 4; i++) advance(l); }
            tl_add(&tl, make_tok(TOK_LINREF, buf, line, col)); continue;
        }

        /* /lang/ us Lu /q[n] — language binding metadata.
           Do not treat every '/' as metadata: plain '/' is division. */
        if (starts_with(l, "/lang/")) {
            for (int i = 0; i < 6; i++) advance(l); /* skip /lang/ */
            char buf[128]; int bi = 0;
            while (cur(l) && cur(l) != '\n' && bi < (int)sizeof(buf) - 1) {
                buf[bi++] = cur(l);
                advance(l);
            }
            buf[bi] = '\0';
            tl_add(&tl, make_tok(TOK_LANG_BIND, buf, line, col)); continue;
        }

        /* ── Keywords that are plain words ── */
        if (isalpha((unsigned char)c) || c == '_') {
            char *w = read_word(l);
            TokenType t = keyword_type(w);
            tl_add(&tl, make_tok(t, w, line, col)); free(w); continue;
        }

        /* ── Single-char operators / punctuation ── */
        {
            char s[2] = {c, '\0'};
            TokenType t = TOK_UNKNOWN;
            switch (c) {
                case '+': t = TOK_PLUS; break;
                case '-': t = TOK_MINUS; break;
                case '*': t = TOK_STAR; break;
                case '/': t = TOK_SLASH; break;
                case '%': t = TOK_PERCENT; break;
                case '&': t = TOK_AMP; break;
                case '|': t = TOK_PIPE; break;
                case '^': t = TOK_CARET; break;
                case '~': t = TOK_TILDE; break;
                case '!': t = TOK_BANG; break;
                case '<': t = TOK_LT; break;
                case '>': t = TOK_GT; break;
                case '?': t = TOK_QUESTION; break;
                case ':': t = TOK_COLON; break;
                case '.': t = TOK_DOT; break;
                case ',': t = TOK_COMMA; break;
                case ';': t = TOK_SEMICOLON; break;
                case '=': t = TOK_ASSIGN; break;
                case '(': t = TOK_LPAREN; break;
                case ')': t = TOK_RPAREN; break;
                case '{': t = TOK_LBRACE; break;
                case '}': t = TOK_RBRACE; break;
                case '[': t = TOK_LBRACKET; break;
                case ']': t = TOK_RBRACKET; break;
                default: break;
            }
            advance(l);
            tl_add(&tl, make_tok(t, s, line, col)); continue;
        }
    }
    tl_add(&tl, make_tok(TOK_EOF, "", l->line, l->col));
    *out_count = tl.count;
    return tl.data;
}

void tokens_free(Token *toks, int count) {
    for (int i = 0; i < count; i++) free(toks[i].value);
    free(toks);
}

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_LANG_DECL: return "LANG_DECL";
        case TOK_IMPORT: return "IMPORT";
        case TOK_BLOCK_ID: return "BLOCK_ID";
        case TOK_IF: return "IF";
        case TOK_PR: return "PR";
        case TOK_TO: return "TO";
        case TOK_FUNC: return "FUNC";
        case TOK_CALL: return "CALL";
        case TOK_RETURN: return "RETURN";
        case TOK_LOOP: return "LOOP";
        case TOK_SERVER: return "SERVER";
        case TOK_USER: return "USER";
        case TOK_COR: return "COR";
        case TOK_SEND: return "SEND";
        case TOK_IDENT: return "IDENT";
        case TOK_INT_LIT: return "INT";
        case TOK_STR_LIT: return "STR";
        case TOK_BOOL_LIT: return "BOOL";
        case TOK_EOF: return "EOF";
        default: return "TOK";
    }
}