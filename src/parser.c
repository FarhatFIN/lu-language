#include "lu.h"

/* ─────────────────────────────────────────────
   Forward declarations
   ───────────────────────────────────────────── */
static char *consume_type(Parser *p);  /* defined after parse_primary */

/* ─────────────────────────────────────────────
   Parser helpers
   ───────────────────────────────────────────── */
static Token *cur(Parser *p) {
    /* Bounds check: if pos went past the end (shouldn't happen normally
       because consume() clamps, but error recovery can overshoot), return
       the EOF token. */
    if (p->pos >= p->count) return &p->tokens[p->count - 1];
    return &p->tokens[p->pos];
}
static Token *peek_tok(Parser *p, int off) {
    int idx = p->pos + off;
    if (idx < 0) idx = 0;
    if (idx >= p->count) return &p->tokens[p->count - 1];
    return &p->tokens[idx];
}
static Token *consume(Parser *p) {
    Token *t = cur(p);
    if (p->pos < p->count - 1) p->pos++;
    return t;
}
static bool check(Parser *p, TokenType t) { return cur(p)->type == t; }
static bool match(Parser *p, TokenType t) {
    if (check(p, t)) { consume(p); return true; } return false;
}
static void skip_newlines(Parser *p) {
    while (check(p, TOK_NEWLINE)) consume(p);
}
static Token *expect(Parser *p, TokenType t, const char *ctx) {
    if (!check(p, t)) {
        /* Error recovery: warn instead of exit, and try to continue.
           But increment the error counter so main() returns exit code 1. */
        g_parse_error_count++;
        lu_warn(cur(p)->line, "expected %s near '%s' in %s (skipping)",
                 token_type_name(t), cur(p)->value ? cur(p)->value : "?", ctx);
        return cur(p);
    }
    return consume(p);
}
/* consume a single newline if present */
static void opt_newline(Parser *p) { if (check(p, TOK_NEWLINE)) consume(p); }

/* ─────────────────────────────────────────────
   Forward declarations
   ───────────────────────────────────────────── */
static ASTNode *parse_statement(Parser *p);
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_block_body(Parser *p);
static bool is_type_tok(TokenType t);
static ASTNode *parse_postfix(Parser *p);
static char *consume_type(Parser *p);
static void parse_fn_param_types(Parser *p, char *buf, size_t cap);

/* Temporary list for collecting lambda parameters during parsing. */
static NodeList n_lambda_params = {0};

/* ─────────────────────────────────────────────
   F-string v2: parse the content of f"..." into a NODE_FSTRING
   with children = list of parts.
   Each part is an ASTNode with:
     - annot = "lit"  → sval is the literal text
     - annot = "expr" → children[0] is the parsed expression AST
   The expression inside {} is parsed by creating a temporary
   Lexer + Parser on the substring and calling parse_expr.
   ───────────────────────────────────────────── */
static ASTNode *parse_fstring(const char *content, int line) {
    ASTNode *fnode = node_new(NODE_FSTRING, line);

    int i = 0;
    int len = (int)strlen(content);
    char litbuf[1024];
    int litlen = 0;

    while (i < len) {
        if (content[i] == '{') {
            /* flush literal part if any */
            if (litlen > 0) {
                litbuf[litlen] = '\0';
                ASTNode *part = node_new(NODE_LITERAL_STR, line);
                part->annot = lu_strdup("lit");
                part->sval = lu_strdup(litbuf);
                node_list_add(&fnode->children, part);
                litlen = 0;
            }
            /* find matching } */
            int j = i + 1;
            int depth = 1;
            while (j < len && depth > 0) {
                if (content[j] == '{') depth++;
                else if (content[j] == '}') { depth--; if (depth == 0) break; }
                j++;
            }
            if (depth == 0) {
                int elen = j - i - 1;
                /* extract the expression substring */
                char *expr_str = (char*)malloc(elen + 1);
                memcpy(expr_str, content + i + 1, elen);
                expr_str[elen] = '\0';
                /* parse the expression using a temporary lexer+parser */
                Lexer *lex = lexer_new(expr_str);
                int tok_count = 0;
                Token *toks = lexer_tokenize(lex, &tok_count);
                Parser tp = { toks, tok_count, 0 };
                ASTNode *expr_ast = parse_expr(&tp);
                tokens_free(toks, tok_count);
                lexer_free(lex);
                free(expr_str);
                /* wrap in a part node */
                ASTNode *part = node_new(NODE_LITERAL_STR, line);
                part->annot = lu_strdup("expr");
                if (expr_ast) node_list_add(&part->children, expr_ast);
                node_list_add(&fnode->children, part);
                i = j + 1;
            } else {
                /* no closing } — treat { as literal */
                litbuf[litlen++] = content[i++];
            }
        } else if (content[i] == '}') {
            /* stray } — treat as literal */
            litbuf[litlen++] = content[i++];
            if (litlen >= (int)sizeof(litbuf) - 1) {
                litbuf[litlen] = '\0';
                ASTNode *part = node_new(NODE_LITERAL_STR, line);
                part->annot = lu_strdup("lit");
                part->sval = lu_strdup(litbuf);
                node_list_add(&fnode->children, part);
                litlen = 0;
            }
        } else {
            litbuf[litlen++] = content[i++];
            if (litlen >= (int)sizeof(litbuf) - 1) {
                litbuf[litlen] = '\0';
                ASTNode *part = node_new(NODE_LITERAL_STR, line);
                part->annot = lu_strdup("lit");
                part->sval = lu_strdup(litbuf);
                node_list_add(&fnode->children, part);
                litlen = 0;
            }
        }
    }
    /* flush trailing literal */
    if (litlen > 0) {
        litbuf[litlen] = '\0';
        ASTNode *part = node_new(NODE_LITERAL_STR, line);
        part->annot = lu_strdup("lit");
        part->sval = lu_strdup(litbuf);
        node_list_add(&fnode->children, part);
    }
    return fnode;
}

/* ─────────────────────────────────────────────
   Expression parser (recursive descent)
   ───────────────────────────────────────────── */
static ASTNode *parse_primary(Parser *p) {
    skip_newlines(p);
    Token *t = cur(p);
    int line = t->line;

    /* Literals */
    if (t->type == TOK_INT_LIT) {
        ASTNode *n = node_new(NODE_LITERAL_INT, line);
        const char *v = t->value ? t->value : "0";
        if (v[0] == '0' && (v[1] == 'b' || v[1] == 'B'))
            n->ival = strtoll(v + 2, NULL, 2);
        else if (v[0] == '0' && (v[1] == 'o' || v[1] == 'O'))
            n->ival = strtoll(v + 2, NULL, 8);
        else
            n->ival = strtoll(v, NULL, 0); /* decimal and 0x hex */
        consume(p); return n;
    }
    if (t->type == TOK_FLOAT_LIT) {
        ASTNode *n = node_new(NODE_LITERAL_FLOAT, line);
        n->fval = atof(t->value); consume(p); return n;
    }
    if (t->type == TOK_STR_LIT) {
        const char *v = t->value ? t->value : "";
        /* Check for f-string marker: starts with "$f" */
        if (v[0] == '$' && v[1] == 'f') {
            /* Parse the f-string into parts (literals + expressions) */
            ASTNode *n = parse_fstring(v + 2, line);
            consume(p);
            return n;
        }
        ASTNode *n = node_new(NODE_LITERAL_STR, line);
        n->sval = lu_strdup(v); consume(p); return n;
    }
    if (t->type == TOK_BOOL_LIT) {
        ASTNode *n = node_new(NODE_LITERAL_BOOL, line);
        n->bval = !strcmp(t->value, "true"); consume(p); return n;
    }

    /* null literal — for optionals (?T) and pointers */
    if (t->type == TOK_IDENT && !strcmp(t->value, "null")) {
        ASTNode *n = node_new(NODE_LITERAL_INT, line);
        n->ival = 0;
        n->sval = lu_strdup("null");  /* mark as null for codegen */
        consume(p);
        return n;
    }

    /* Grouped expression OR type cast (type)expr.
       If the next token after ( is a type keyword and the one after that
       is ), it's a cast: (int)x, (float)x, (byte)x, (str)x, etc. */
    if (t->type == TOK_LPAREN) {
        /* Peek: ( type ) ... */
        if (is_type_tok(peek_tok(p, 1)->type) && peek_tok(p, 2)->type == TOK_RPAREN) {
            consume(p); /* ( */
            char *tname = consume_type(p);
            match(p, TOK_RPAREN); /* ) */
            /* The operand — parse as a unary (so (int)x works on a single
               identifier, deref, etc.). We use parse_postfix to allow
               (int)x.field and (int)*p. */
            ASTNode *operand = parse_postfix(p);
            ASTNode *n = node_new(NODE_EXPR_UNOP, line);
            char op_buf[128];
            snprintf(op_buf, sizeof(op_buf), "(cast:%s)", tname);
            n->op = lu_strdup(op_buf);
            n->type_name = tname;
            node_list_add(&n->children, operand);
            return n;
        }
        consume(p);
        ASTNode *e = parse_expr(p);
        match(p, TOK_RPAREN);
        return e;
    }

    /* List literal: [1, 2, 3] → lowered to a static C array.
       We emit it as a NODE_VAR_DECL-like with array marker, but since
       parse_primary returns an expression, we wrap it in a special node. */
    if (t->type == TOK_LBRACKET) {
        consume(p); /* [ */
        ASTNode *n = node_new(NODE_EXPR_INDEX, line); /* reuse: list literal */
        n->op = lu_strdup("list");
        while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
            node_list_add(&n->children, parse_expr(p));
            match(p, TOK_COMMA);
        }
        match(p, TOK_RBRACKET);
        return n;
    }

    /* Unary operators */
    if (t->type == TOK_BANG || t->type == TOK_MINUS || t->type == TOK_TILDE ||
        t->type == TOK_INC || t->type == TOK_DEC) {
        ASTNode *n = node_new(NODE_EXPR_UNOP, line);
        n->op = lu_strdup(t->value); consume(p);
        node_list_add(&n->children, parse_primary(p));
        return n;
    }

    /* *p — C-style dereference */
    if (t->type == TOK_STAR) {
        consume(p);
        ASTNode *n = node_new(NODE_EXPR_DEREF, line);
        node_list_add(&n->children, parse_primary(p));
        return n;
    }

    /* &x — C-style address-of */
    if (t->type == TOK_AMP) {
        consume(p);
        ASTNode *n = node_new(NODE_EXPR_REF, line);
        node_list_add(&n->children, parse_primary(p));
        return n;
    }

    /* Deref/ */
    if (t->type == TOK_DEREF) {
        consume(p);
        ASTNode *n = node_new(NODE_EXPR_DEREF, line);
        node_list_add(&n->children, parse_primary(p));
        return n;
    }

    /* Ref/ */
    if (t->type == TOK_REF) {
        consume(p);
        ASTNode *n = node_new(NODE_EXPR_REF, line);
        node_list_add(&n->children, parse_primary(p));
        return n;
    }

    /* Alloc/ used as expression: Alloc/type:count */
    if (t->type == TOK_ALLOC) {
        consume(p);
        ASTNode *n = node_new(NODE_ALLOC, line);
        n->type_name = consume_type(p);
        match(p, TOK_COLON);
        node_list_add(&n->children, parse_expr(p));
        return n;
    }

    /* Call/ inside expression */
    if (t->type == TOK_CALL) {
        consume(p);
        ASTNode *n = node_new(NODE_FUNC_CALL, line);
        n->sval = lu_strdup(cur(p)->value); consume(p);
        match(p, TOK_LPAREN);
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
            node_list_add(&n->children, parse_expr(p));
            match(p, TOK_COMMA);
        }
        match(p, TOK_RPAREN);
        return n;
    }

    /* new Type(args...) as expression, C++-style heap allocation */
    if (t->type == TOK_IDENT && t->value && !strcmp(t->value, "new")) {
        consume(p);
        ASTNode *n = node_new(NODE_NEW_EXPR, line);
        if (!check(p, TOK_EOF)) { n->sval = lu_strdup(cur(p)->value ? cur(p)->value : "void"); consume(p); }
        if (check(p, TOK_LT)) {
            consume(p);
            n->annot = lu_strdup(cur(p)->value ? cur(p)->value : "");
            consume(p);
            match(p, TOK_GT);
        }
        match(p, TOK_LPAREN);
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
            node_list_add(&n->children, parse_expr(p));
            match(p, TOK_COMMA);
        }
        match(p, TOK_RPAREN);
        return n;
    }

    /* Lambda: fn(params) -> ret { body }
       Generates a static function with a unique name and returns its
       address. The lambda's AST is stored as annot="lambda" with the
       body in children, the signature in type_name, and param names
       as child NODE_VAR_DECL nodes (like a regular function). */
    if (t->type == TOK_IDENT && t->value && !strcmp(t->value, "fn") &&
        peek_tok(p, 1)->type == TOK_LPAREN) {
        consume(p); /* eat 'fn' */
        /* Parse params: (type name, type name, ...). Collect both the
           type signature (for the function pointer type) and the param
           names (as NODE_VAR_DECL children, like a regular function). */
        char sig_buf[256] = "fn:(";
        match(p, TOK_LPAREN);
        bool first = true;
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
            if (!first) strncat(sig_buf, ",", sizeof(sig_buf) - strlen(sig_buf) - 1);
            char *ptype = consume_type(p);
            strncat(sig_buf, ptype, sizeof(sig_buf) - strlen(sig_buf) - 1);
            first = false;
            /* Parse optional param name and store as NODE_VAR_DECL. */
            if (check(p, TOK_IDENT)) {
                ASTNode *param = node_new(NODE_VAR_DECL, line);
                param->type_name = lu_strdup(ptype);
                param->sval = lu_strdup(cur(p)->value);
                consume(p);
                node_list_add(&n_lambda_params, param);
            }
            free(ptype);
            match(p, TOK_COMMA);
        }
        strncat(sig_buf, ")", sizeof(sig_buf) - strlen(sig_buf) - 1);
        match(p, TOK_RPAREN);
        /* Return type. */
        if (match(p, TOK_PTR_ACCESS) || match(p, TOK_COLON)) {
            strncat(sig_buf, "->", sizeof(sig_buf) - strlen(sig_buf) - 1);
            char *ret = consume_type(p);
            strncat(sig_buf, ret, sizeof(sig_buf) - strlen(sig_buf) - 1);
            free(ret);
        } else {
            strncat(sig_buf, "->void", sizeof(sig_buf) - strlen(sig_buf) - 1);
        }
        ASTNode *n = node_new(NODE_FUNC_DECL, line);
        n->annot = lu_strdup("lambda");
        n->type_name = lu_strdup(sig_buf);
        n->op = lu_strdup(sig_buf);
        /* Move the collected params into this lambda's children (before
           the body, which we'll add next). */
        for (int i = 0; i < n_lambda_params.count; i++)
            node_list_add(&n->children, n_lambda_params.items[i]);
        n_lambda_params.count = 0;
        /* Parse the body { } */
        skip_newlines(p);
        if (match(p, TOK_LBRACE)) {
            ASTNode *body = node_new(NODE_PROGRAM, line);
            skip_newlines(p);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                skip_newlines(p);
                if (check(p, TOK_RBRACE)) break;
                int pos_before = p->pos;
                ASTNode *s = parse_statement(p);
                if (s) node_list_add(&body->children, s);
                opt_newline(p);
                if (p->pos == pos_before) consume(p);
            }
            match(p, TOK_RBRACE);
            node_list_add(&n->children, body);
        }
        return n;
    }

    /* Identifier (possibly with dot/bracket access) */
    if (t->type == TOK_IDENT || t->type == TOK_TYPE_INT || t->type == TOK_TYPE_FLOAT ||
        t->type == TOK_TYPE_STR || t->type == TOK_TYPE_BOOL || t->type == TOK_TYPE_BYTE ||
        t->type == TOK_TYPE_IP || t->type == TOK_TYPE_ID || t->type == TOK_TYPE_MSG ||
        t->type == TOK_TYPE_COR || t->type == TOK_TYPE_INT64 || t->type == TOK_TYPE_LIB ||
        t->type == TOK_TYPE_VOID) {
        ASTNode *n = node_new(NODE_IDENT, line);
        n->sval = lu_strdup(t->value); consume(p);

        /* function call: ident(...) */
        if (check(p, TOK_LPAREN)) {
            ASTNode *call = node_new(NODE_FUNC_CALL, line);
            call->sval = lu_strdup(n->sval);
            ast_free(n);
            consume(p); /* ( */
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
                node_list_add(&call->children, parse_expr(p));
                match(p, TOK_COMMA);
            }
            match(p, TOK_RPAREN);
            n = call;
        }

        /* field access: n.field or n->field or n[idx] or n[start:end] (slice) */
        while (check(p, TOK_DOT) || check(p, TOK_PTR_ACCESS) || check(p, TOK_LBRACKET)) {
            if (check(p, TOK_DOT) || check(p, TOK_PTR_ACCESS)) {
                bool ptr = check(p, TOK_PTR_ACCESS);
                consume(p);
                ASTNode *fa = node_new(NODE_EXPR_FIELD, line);
                fa->op = lu_strdup(ptr ? "->" : ".");
                fa->sval = lu_strdup(cur(p)->value); consume(p);
                node_list_add(&fa->children, n);
                n = fa;
            } else { /* [ index ] or [ start : end ] (slice) */
                consume(p);
                ASTNode *first = parse_expr(p);
                if (match(p, TOK_COLON)) {
                    /* Slice: n[start:end] → NODE_EXPR_INDEX with op="slice"
                       children[0] = n, children[1] = start, children[2] = end */
                    ASTNode *end_expr = parse_expr(p);
                    ASTNode *ia = node_new(NODE_EXPR_INDEX, line);
                    ia->op = lu_strdup("slice");
                    node_list_add(&ia->children, n);
                    node_list_add(&ia->children, first);
                    node_list_add(&ia->children, end_expr);
                    n = ia;
                } else {
                    /* Regular index */
                    ASTNode *ia = node_new(NODE_EXPR_INDEX, line);
                    node_list_add(&ia->children, n);
                    node_list_add(&ia->children, first);
                    n = ia;
                }
                match(p, TOK_RBRACKET);
            }
        }
        return n;
    }

    /* cor key as literal: cor/{...} was already tokenized as TOK_COR */
    if (t->type == TOK_COR) {
        ASTNode *n = node_new(NODE_LITERAL_COR, line);
        n->sval = lu_strdup(t->value); consume(p); return n;
    }

    /* Block id reference */
    if (t->type == TOK_BLOCK_ID) {
        ASTNode *n = node_new(NODE_IDENT, line);
        char buf[32]; snprintf(buf, sizeof(buf), "_q%s", t->value);
        n->sval = lu_strdup(buf); consume(p); return n;
    }

    /* fallback */
    ASTNode *n = node_new(NODE_IDENT, line);
    n->sval = lu_strdup(t->value ? t->value : "?");
    consume(p);
    return n;
}

/* Postfix ++/-- */
static ASTNode *parse_postfix(Parser *p) {
    ASTNode *n = parse_primary(p);
    while (check(p, TOK_INC) || check(p, TOK_DEC)) {
        ASTNode *u = node_new(NODE_EXPR_UNOP, cur(p)->line);
        char op[16]; snprintf(op, sizeof(op), "%s_post", cur(p)->value);
        u->op = lu_strdup(op); consume(p);
        node_list_add(&u->children, n);
        n = u;
    }
    return n;
}

/* Binary with precedence climbing */
static int binop_prec(TokenType t) {
    switch (t) {
        case TOK_DSTAR:   return 12;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 11;
        case TOK_PLUS: case TOK_MINUS: return 10;
        case TOK_LSHIFT: case TOK_RSHIFT: case TOK_URSHIFT: return 9;
        case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: return 8;
        case TOK_EQ: case TOK_NEQ: return 7;
        case TOK_AMP: return 6;
        case TOK_CARET: return 5;
        case TOK_PIPE: return 4;
        case TOK_AND: return 3;
        case TOK_OR: return 2;
        default: return -1;
    }
}

static ASTNode *parse_binop(Parser *p, int min_prec) {
    ASTNode *left = parse_postfix(p);
    while (true) {
        int prec = binop_prec(cur(p)->type);
        if (prec < min_prec) break;
        Token *op = consume(p);
        ASTNode *right = parse_binop(p, prec + 1);
        ASTNode *n = node_new(NODE_EXPR_BINOP, op->line);
        n->op = lu_strdup(op->value);
        node_list_add(&n->children, left);
        node_list_add(&n->children, right);
        left = n;
    }
    return left;
}

/* Ternary */
static ASTNode *parse_expr(Parser *p) {
    ASTNode *cond = parse_binop(p, 0);
    if (match(p, TOK_QUESTION)) {
        ASTNode *n = node_new(NODE_EXPR_TERNARY, cond->line);
        node_list_add(&n->children, cond);
        node_list_add(&n->children, parse_expr(p));
        expect(p, TOK_COLON, "ternary");
        node_list_add(&n->children, parse_expr(p));
        return n;
    }
    return cond;
}

/* ─────────────────────────────────────────────
   Type parsing helpers
   ───────────────────────────────────────────── */
static bool is_type_tok(TokenType t) {
    return t == TOK_TYPE_INT  || t == TOK_TYPE_INT64 || t == TOK_TYPE_FLOAT ||
           t == TOK_TYPE_STR  || t == TOK_TYPE_BOOL  || t == TOK_TYPE_BYTE  ||
           t == TOK_TYPE_IP   || t == TOK_TYPE_ID    || t == TOK_TYPE_COR   ||
           t == TOK_TYPE_MSG  || t == TOK_TYPE_LIB   || t == TOK_TYPE_VOID  ||
           t == TOK_IDENT; /* user-defined types */
}

/* Parse a parameter type list for fn types: (int, str) */
static void parse_fn_param_types(Parser *p, char *buf, size_t cap) {
    /* buf does NOT yet contain "(" — add it */
    strncat(buf, "(", cap - strlen(buf) - 1);
    bool first = true;
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
        if (!first) strncat(buf, ",", cap - strlen(buf) - 1);
        char *t = consume_type(p);
        strncat(buf, t, cap - strlen(buf) - 1);
        free(t);
        first = false;
        match(p, TOK_COMMA);
    }
    strncat(buf, ")", cap - strlen(buf) - 1);
}

static char *consume_type(Parser *p) {
    /* Optional type: ?T → stores as "?T" */
    if (check(p, TOK_QUESTION)) {
        consume(p); /* eat '?' */
        char *inner = consume_type(p);
        char buf[128];
        snprintf(buf, sizeof(buf), "?%s", inner);
        free(inner);
        return lu_strdup(buf);
    }
    /* Error union type: !T → stores as "!T" */
    if (check(p, TOK_BANG)) {
        consume(p); /* eat '!' */
        char *inner = consume_type(p);
        char buf[128];
        snprintf(buf, sizeof(buf), "!%s", inner);
        free(inner);
        return lu_strdup(buf);
    }
    /* Function pointer type: fn(args) -> ret
       Stored as "fn:paramtypes->rettype" e.g. "fn:int,int->int".
       Codegen lowers this to ret (*name)(params). */
    if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "fn")) {
        consume(p); /* eat 'fn' */
        char buf[256] = "fn:";
        if (match(p, TOK_LPAREN)) {
            parse_fn_param_types(p, buf, sizeof(buf));
            match(p, TOK_RPAREN);
        } else {
            strncat(buf, "()", sizeof(buf) - strlen(buf) - 1);
        }
        /* Optional return type: -> ret or : ret */
        if (match(p, TOK_PTR_ACCESS) || match(p, TOK_COLON)) {
            strncat(buf, "->", sizeof(buf) - strlen(buf) - 1);
            char *ret = consume_type(p);
            strncat(buf, ret, sizeof(buf) - strlen(buf) - 1);
            free(ret);
        } else {
            strncat(buf, "->void", sizeof(buf) - strlen(buf) - 1);
        }
        return lu_strdup(buf);
    }
    if (!is_type_tok(cur(p)->type)) return lu_strdup("auto");
    char *s = lu_strdup(cur(p)->value);
    consume(p);
    /* Template parameter: Vector<int> → "Vector<int>" */
    if (check(p, TOK_LT)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s<", s);
        free(s);
        consume(p); /* < */
        while (!check(p, TOK_GT) && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
            if (cur(p)->value) {
                strncat(buf, cur(p)->value, sizeof(buf) - strlen(buf) - 1);
                consume(p);
            } else break;
        }
        if (check(p, TOK_GT)) {
            strncat(buf, ">", sizeof(buf) - strlen(buf) - 1);
            consume(p);
        }
        s = lu_strdup(buf);
    }
    /* Pointer type suffix: int*, void*, str* → append "*" to type name.
       This lets users write `void* data` or `int* ptr` in function params. */
    while (check(p, TOK_STAR)) {
        consume(p);
        char pbuf[256];
        snprintf(pbuf, sizeof(pbuf), "%s*", s);
        free(s);
        s = lu_strdup(pbuf);
    }
    return s;
}

static bool is_ident_like(TokenType t) {
    return t == TOK_IDENT || t == TOK_TYPE_INT || t == TOK_TYPE_INT64 ||
           t == TOK_TYPE_FLOAT || t == TOK_TYPE_STR || t == TOK_TYPE_BOOL ||
           t == TOK_TYPE_BYTE || t == TOK_TYPE_IP || t == TOK_TYPE_ID ||
           t == TOK_TYPE_COR || t == TOK_TYPE_MSG || t == TOK_TYPE_LIB ||
           t == TOK_TYPE_VOID;
}

static void append_token_text(char *buf, size_t cap, const char *text) {
    if (!text || cap == 0) return;
    size_t used = strlen(buf);
    if (used >= cap - 1) return;
    strncat(buf, text, cap - used - 1);
}

static ASTNode *parse_import_like(Parser *p, NodeKind kind, int line) {
    consume(p); /* Import or #include */
    ASTNode *n = node_new(kind, line);

    if (check(p, TOK_STR_LIT)) {
        char buf[512];
        snprintf(buf, sizeof(buf), "\"%s\"", cur(p)->value ? cur(p)->value : "");
        n->sval = lu_strdup(buf);
        consume(p);
    } else if (check(p, TOK_LT)) {
        char buf[512] = "<";
        consume(p); /* < */
        while (!check(p, TOK_GT) && !check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
            append_token_text(buf, sizeof(buf), cur(p)->value);
            consume(p);
        }
        if (check(p, TOK_GT)) {
            append_token_text(buf, sizeof(buf), ">");
            consume(p);
        }
        n->sval = lu_strdup(buf);
    } else if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
        /* Bare import, e.g. Import Russian: keep it as metadata, not C include. */
        n->sval = lu_strdup(cur(p)->value ? cur(p)->value : "");
        consume(p);
    } else {
        n->sval = lu_strdup("");
    }

    /* Consume any trailing tokens on the import line so angle includes and
       language imports don't leak into the AST as stray expressions. */
    while (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) consume(p);
    return n;
}

/* ─────────────────────────────────────────────
   Statement parsing
   ───────────────────────────────────────────── */

/* Parse param list: int a, str b ... */
static void parse_params(Parser *p, ASTNode *fn) {
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_RPAREN) || check(p, TOK_EOF)) break;

        int before = p->pos;
        ASTNode *param = node_new(NODE_VAR_DECL, cur(p)->line);
        param->type_name = consume_type(p);

        /* Accept any non-separator token as the parameter name.
           This keeps names like `exp` valid even though the lexer also has
           TOK_EXP for the standalone game command. */
        if (!check(p, TOK_COMMA) && !check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            param->sval = lu_strdup(cur(p)->value ? cur(p)->value : "_param");
            consume(p);
        } else {
            param->sval = lu_strdup("_param");
        }

        node_list_add(&fn->children, param);
        match(p, TOK_COMMA);
        if (p->pos == before) consume(p); /* hard safety: malformed params cannot hang */
    }
}

static ASTNode *parse_if(Parser *p) {
    int line = cur(p)->line; consume(p); /* If/ or Elif/ */
    ASTNode *n = node_new(NODE_IF, line);

    /* condition: read until { or newline */
    ASTNode *cond = parse_expr(p);
    node_list_add(&n->children, cond);

    /* Check for brace-delimited body: If/ cond { ... } */
    skip_newlines(p);
    ASTNode *then_body = node_new(NODE_PROGRAM, line);

    if (check(p, TOK_LBRACE)) {
        /* brace body */
        consume(p); /* { */
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_RBRACE) || check(p, TOK_EOF)) break;
            ASTNode *s = parse_statement(p);
            if (s) node_list_add(&then_body->children, s);
            opt_newline(p);
        }
        if (check(p, TOK_RBRACE)) consume(p); /* } */
    } else {
        /* To/-style or inline body — stop after one To/ statement */
        while (!check(p, TOK_ELIF) && !check(p, TOK_ELSE) &&
               !check(p, TOK_BLOCK_ID) && !check(p, TOK_BLOCK_END) &&
               !check(p, TOK_FUNC) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_ELIF) || check(p, TOK_ELSE) ||
                check(p, TOK_BLOCK_ID) || check(p, TOK_BLOCK_END) ||
                check(p, TOK_FUNC) || check(p, TOK_EOF)) break;
            ASTNode *s = parse_statement(p);
            if (s) node_list_add(&then_body->children, s);
            opt_newline(p);
            /* After a To/ statement, stop the then-body */
            if (then_body->children.count > 0) {
                ASTNode *last = then_body->children.items[then_body->children.count - 1];
                if (last->kind == NODE_TO) break;
            }
        }
    }
    node_list_add(&n->children, then_body);

    /* Elif / Else */
    skip_newlines(p);
    if (check(p, TOK_ELIF)) {
        ASTNode *alt = parse_if(p);
        alt->kind = NODE_IF;
        node_list_add(&n->children, alt);
    } else if (match(p, TOK_ELSE)) {
        opt_newline(p);
        ASTNode *else_body = node_new(NODE_PROGRAM, line);
        if (check(p, TOK_LBRACE)) {
            consume(p); /* { */
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                skip_newlines(p);
                if (check(p, TOK_RBRACE) || check(p, TOK_EOF)) break;
                ASTNode *s = parse_statement(p);
                if (s) node_list_add(&else_body->children, s);
                opt_newline(p);
            }
            if (check(p, TOK_RBRACE)) consume(p);
        } else {
            skip_newlines(p);
            if (!check(p, TOK_EOF) && !check(p, TOK_BLOCK_ID) &&
                !check(p, TOK_BLOCK_END) && !check(p, TOK_FUNC)) {
                ASTNode *s = parse_statement(p);
                if (s) node_list_add(&else_body->children, s);
            }
        }
        node_list_add(&n->children, else_body);
    }
    return n;
}

static ASTNode *parse_loop(Parser *p) {
    int line = cur(p)->line; consume(p); /* Loop/ */
    ASTNode *n;
    /* Check current token BEFORE consuming to avoid pos-1 fragility */
    if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "While")) {
        consume(p); /* eat "While" */
        n = node_new(NODE_LOOP_WHILE, line);
        node_list_add(&n->children, parse_expr(p));
    } else if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "Each")) {
        consume(p); /* eat "Each" */
        n = node_new(NODE_LOOP_EACH, line);
        ASTNode *item = node_new(NODE_IDENT, line);
        item->sval = lu_strdup(cur(p)->value); consume(p); /* item name */
        node_list_add(&n->children, item);
        /* expect 'in' */
        if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "in")) consume(p);
        node_list_add(&n->children, parse_expr(p)); /* list */
    } else {
        /* Loop/N — count is an integer literal or identifier */
        n = node_new(NODE_LOOP, line);
        ASTNode *cnt = node_new(NODE_LITERAL_INT, line);
        if (check(p, TOK_INT_LIT)) {
            cnt->ival = atoll(cur(p)->value);
            consume(p);
        } else if (check(p, TOK_IDENT)) {
            /* Loop/varname — treat ident as count expression */
            free(cnt); /* discard literal node */
            cnt = parse_expr(p);
        } else {
            cnt->ival = 1; /* fallback */
        }
        node_list_add(&n->children, cnt);
    }
    /* body in { } */
    skip_newlines(p);
    if (match(p, TOK_LBRACE)) {
        ASTNode *body = node_new(NODE_PROGRAM, line);
        skip_newlines(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_RBRACE)) break;
            int pos_before = p->pos;
            ASTNode *s = parse_statement(p);
            if (s) node_list_add(&body->children, s);
            opt_newline(p);
            if (p->pos == pos_before) consume(p); /* safety: never stall */
        }
        match(p, TOK_RBRACE);
        node_list_add(&n->children, body);
    }
    return n;
}

static ASTNode *parse_func(Parser *p, bool is_async) {
    int line = cur(p)->line; consume(p); /* Fn/ */
    ASTNode *n = node_new(is_async ? NODE_ASYNC_FUNC : NODE_FUNC_DECL, line);
    n->sval = lu_strdup(cur(p)->value); consume(p); /* name */
    match(p, TOK_LPAREN);
    parse_params(p, n);
    match(p, TOK_RPAREN);
    /* return type */
    if (match(p, TOK_COLON)) n->type_name = consume_type(p);
    skip_newlines(p);
    /* body { } */
    if (match(p, TOK_LBRACE)) {
        ASTNode *body = node_new(NODE_PROGRAM, line);
        skip_newlines(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_RBRACE)) break;
            int pos_before = p->pos;
            ASTNode *s = parse_statement(p);
            if (s) node_list_add(&body->children, s);
            opt_newline(p);
            if (p->pos == pos_before) consume(p); /* safety: never stall */
        }
        match(p, TOK_RBRACE);
        node_list_add(&n->children, body);
    }
    return n;
}

static ASTNode *parse_try(Parser *p) {
    int line = cur(p)->line; consume(p);
    ASTNode *n = node_new(NODE_TRY, line);
    skip_newlines(p);
    match(p, TOK_LBRACE);
    ASTNode *try_body = node_new(NODE_PROGRAM, line);
    skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_RBRACE)) break;
        int pos_before = p->pos;
        node_list_add(&try_body->children, parse_statement(p));
        opt_newline(p);
        if (p->pos == pos_before) consume(p);
    }
    match(p, TOK_RBRACE);
    node_list_add(&n->children, try_body);
    skip_newlines(p);
    /* catch */
    while (match(p, TOK_CATCH)) {
        ASTNode *catch = node_new(NODE_PROGRAM, cur(p)->line);
        catch->sval = lu_strdup(cur(p)->value); consume(p); /* error type */
        skip_newlines(p);
        match(p, TOK_LBRACE);
        skip_newlines(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_RBRACE)) break;
            int pos_before = p->pos;
            node_list_add(&catch->children, parse_statement(p));
            opt_newline(p);
            if (p->pos == pos_before) consume(p);
        }
        match(p, TOK_RBRACE);
        node_list_add(&n->children, catch);
        skip_newlines(p);
    }
    /* finally */
    if (match(p, TOK_FINALLY)) {
        ASTNode *fin = node_new(NODE_PROGRAM, cur(p)->line);
        skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_RBRACE)) break;
            int pos_before = p->pos;
            node_list_add(&fin->children, parse_statement(p));
            opt_newline(p);
            if (p->pos == pos_before) consume(p);
        }
        match(p, TOK_RBRACE);
        node_list_add(&n->children, fin);
    }
    return n;
}

/* ─────────────────────────────────────────────
   OOP PARSERS  (v3: class, template, obj, cl, dam, onl, snd--)
   ───────────────────────────────────────────── */

/* Parse access modifier: pub / prv / prt — returns as sval on member */
static const char *parse_access(Parser *p) {
    if (check(p, TOK_IDENT)) {
        const char *v = cur(p)->value;
        if (!strcmp(v,"pub") || !strcmp(v,"prv") || !strcmp(v,"prt")) {
            const char *acc = lu_strdup(v); consume(p); return acc;
        }
    }
    return "pub"; /* default public */
}

/* Parse class/interface/impl block:
   class Foo extends Bar { ... }
   interface IFoo { ... }
   impl Foo : IFoo { ... }
   template<T> class Vec { ... }
*/
static ASTNode *parse_class(Parser *p) {
    int line = cur(p)->line;
    const char *keyword = lu_strdup(cur(p)->value); consume(p); /* class/interface/impl/template */

    NodeKind kind = NODE_CLASS_DECL;
    if (!strcmp(keyword, "interface")) kind = NODE_INTERFACE_DECL;
    if (!strcmp(keyword, "impl"))      kind = NODE_IMPL_DECL;
    if (!strcmp(keyword, "template"))  kind = NODE_TEMPLATE_DECL;

    ASTNode *n = node_new(kind, line);

    /* template<T> — read type param */
    if (!strcmp(keyword, "template")) {
        if (check(p, TOK_LT)) {
            consume(p); /* < */
            n->annot = lu_strdup(cur(p)->value); consume(p); /* T */
            match(p, TOK_GT); /* > */
            skip_newlines(p);
            /* expect 'class' keyword after template<T> */
            if (check(p, TOK_STRUCT) || (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "class")))
                consume(p);
        }
    }

    /* class name */
    if (check(p, TOK_IDENT)) {
        n->sval = lu_strdup(cur(p)->value); consume(p);
    }

    /* extends BaseClass  or  : Interface */
    if ((check(p, TOK_IDENT) && !strcmp(cur(p)->value, "extends")) || check(p, TOK_COLON)) {
        consume(p); /* skip 'extends' or ':' */
        ASTNode *base = node_new(NODE_INHERIT, line);
        /* collect comma-separated base names */
        char bases[256] = {0};
        while (check(p, TOK_IDENT)) {
            if (bases[0]) strncat(bases, ",", sizeof(bases)-strlen(bases)-1);
            strncat(bases, cur(p)->value, sizeof(bases)-strlen(bases)-1);
            consume(p);
            if (!match(p, TOK_COMMA)) break;
        }
        base->sval = lu_strdup(bases);
        node_list_add(&n->children, base);
    }

    skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_RBRACE)) break;

        int mline = cur(p)->line;
        const char *access = parse_access(p);
        skip_newlines(p);

        /* Constructor: same name as class or 'new' */
        if (check(p, TOK_IDENT) && n->sval &&
            (!strcmp(cur(p)->value, n->sval) || !strcmp(cur(p)->value, "new"))) {
            ASTNode *ctor = node_new(NODE_CONSTRUCTOR, mline);
            ctor->annot = lu_strdup(access);
            consume(p); /* name */
            match(p, TOK_LPAREN);
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                ASTNode *param = node_new(NODE_VAR_DECL, mline);
                param->type_name = consume_type(p);
                if (!check(p, TOK_COMMA) && !check(p, TOK_RPAREN) && !check(p, TOK_EOF)) { param->sval = lu_strdup(cur(p)->value ? cur(p)->value : "_param"); consume(p); }
                node_list_add(&ctor->children, param);
                match(p, TOK_COMMA);
            }
            match(p, TOK_RPAREN); match(p, TOK_COLON);
            skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);
            ASTNode *body = node_new(NODE_PROGRAM, mline);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                skip_newlines(p);
                if (check(p, TOK_RBRACE)) break;
                node_list_add(&body->children, parse_statement(p));
                opt_newline(p);
            }
            match(p, TOK_RBRACE);
            node_list_add(&ctor->children, body);
            node_list_add(&n->children, ctor);
            opt_newline(p); continue;
        }

        /* Destructor: ~ClassName or del */
        if ((check(p, TOK_TILDE)) ||
            (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "del"))) {
            ASTNode *dtor = node_new(NODE_DESTRUCTOR, mline);
            dtor->annot = lu_strdup(access);
            consume(p); /* ~ or del */
            if (check(p, TOK_IDENT)) consume(p); /* name */
            if (check(p, TOK_LPAREN)) { match(p, TOK_LPAREN); match(p, TOK_RPAREN); }
            match(p, TOK_COLON);
            skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);
            ASTNode *body = node_new(NODE_PROGRAM, mline);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                skip_newlines(p);
                if (check(p, TOK_RBRACE)) break;
                node_list_add(&body->children, parse_statement(p));
                opt_newline(p);
            }
            match(p, TOK_RBRACE);
            node_list_add(&dtor->children, body);
            node_list_add(&n->children, dtor);
            opt_newline(p); continue;
        }

        /* Method: virtual/override prefix, then Fn/ */
        bool is_virtual = false, is_override = false;
        if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "virtual"))  { is_virtual  = true; consume(p); }
        if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "override")) { is_override = true; consume(p); }

        if (check(p, TOK_FUNC)) {
            ASTNode *method = parse_func(p, false);
            method->kind = NODE_METHOD_DECL;
            method->annot = lu_strdup(access);
            if (is_virtual)  method->bval = true;
            if (is_override) method->ival = 1;
            node_list_add(&n->children, method);
            opt_newline(p); continue;
        }

        /* op overload: op+(other):Type { } */
        if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "op")) {
            consume(p);
            ASTNode *op = node_new(NODE_OP_OVERLOAD, mline);
            op->op = lu_strdup(cur(p)->value); consume(p); /* operator symbol */
            op->annot = lu_strdup(access);
            match(p, TOK_LPAREN);
            ASTNode *param = node_new(NODE_VAR_DECL, mline);
            param->type_name = consume_type(p);
            if (!check(p, TOK_COMMA) && !check(p, TOK_RPAREN) && !check(p, TOK_EOF)) { param->sval = lu_strdup(cur(p)->value ? cur(p)->value : "_param"); consume(p); }
            node_list_add(&op->children, param);
            match(p, TOK_RPAREN); match(p, TOK_COLON);
            op->type_name = lu_strdup(cur(p)->value); consume(p);
            skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);
            ASTNode *body = node_new(NODE_PROGRAM, mline);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                skip_newlines(p);
                if (check(p, TOK_RBRACE)) break;
                node_list_add(&body->children, parse_statement(p));
                opt_newline(p);
            }
            match(p, TOK_RBRACE);
            node_list_add(&op->children, body);
            node_list_add(&n->children, op);
            opt_newline(p); continue;
        }

        /* Field: type name [= value] or type name[size][size]... */
        if (is_type_tok(cur(p)->type) || check(p, TOK_IDENT)) {
            ASTNode *field = node_new(NODE_VAR_DECL, mline);
            field->annot   = lu_strdup(access);
            field->type_name = consume_type(p);
            if (check(p, TOK_IDENT)) { field->sval = lu_strdup(cur(p)->value); consume(p); }
            /* optional [size] for array fields — supports multi-dimensional */
            int cfndims = 0;
            while (match(p, TOK_LBRACKET)) {
                if (cfndims == 0) field->op = lu_strdup("array");
                node_list_add(&field->children, parse_expr(p));
                match(p, TOK_RBRACKET);
                cfndims++;
            }
            field->ival = cfndims;
            if (match(p, TOK_ASSIGN)) {
                node_list_add(&field->children, parse_expr(p));
            }
            node_list_add(&n->children, field);
            opt_newline(p); continue;
        }

        /* fallback: skip unknown token */
        consume(p); opt_newline(p);
    }
    match(p, TOK_RBRACE);
    return n;
}

static ASTNode *parse_struct(Parser *p) {
    int line = cur(p)->line;
    /* Check if this is a union (the caller may have routed a TOK_UNION here) */
    bool is_union = (cur(p)->type == TOK_UNION);
    consume(p);
    ASTNode *n = node_new(is_union ? NODE_UNION_DECL : NODE_STRUCT_DECL, line);
    n->sval = lu_strdup(cur(p)->value); consume(p);
    skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_RBRACE)) break;
        int pos_before = p->pos;
        ASTNode *field = node_new(NODE_VAR_DECL, cur(p)->line);
        field->type_name = consume_type(p);
        if (is_type_tok(cur(p)->type) || check(p, TOK_IDENT)) {
            field->sval = lu_strdup(cur(p)->value); consume(p);
        }
        /* optional [size] for array fields — supports multi-dimensional */
        int fndims = 0;
        while (match(p, TOK_LBRACKET)) {
            if (fndims == 0) field->op = lu_strdup("array");
            node_list_add(&field->children, parse_expr(p));
            match(p, TOK_RBRACKET);
            fndims++;
        }
        field->ival = fndims;
        /* optional = default value */
        if (match(p, TOK_ASSIGN)) {
            node_list_add(&field->children, parse_expr(p));
        }
        node_list_add(&n->children, field);
        opt_newline(p);
        if (p->pos == pos_before) consume(p); /* safety: never stall */
    }
    match(p, TOK_RBRACE);
    return n;
}

static ASTNode *parse_enum(Parser *p) {
    int line = cur(p)->line; consume(p);
    ASTNode *n = node_new(NODE_ENUM_DECL, line);
    n->sval = lu_strdup(cur(p)->value); consume(p);
    skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_RBRACE)) break;
        ASTNode *member = node_new(NODE_IDENT, cur(p)->line);
        member->sval = lu_strdup(cur(p)->value); consume(p);
        if (match(p, TOK_ASSIGN)) {
            const char *mv = cur(p)->value ? cur(p)->value : "0";
            member->ival = strtoll(mv, NULL, 0);
            member->bval = true; /* explicit initializer, including = 0 */
            consume(p);
        }
        node_list_add(&n->children, member);
        match(p, TOK_COMMA); opt_newline(p);
    }
    match(p, TOK_RBRACE);
    return n;
}

static ASTNode *parse_namespace(Parser *p) {
    int line = cur(p)->line; consume(p);
    ASTNode *n = node_new(NODE_NAMESPACE, line);
    n->sval = lu_strdup(cur(p)->value); consume(p);
    skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_RBRACE)) break;
        node_list_add(&n->children, parse_statement(p));
        opt_newline(p);
    }
    match(p, TOK_RBRACE);
    return n;
}

/* Parse an lvalue assignment: x = expr, obj.field = expr, arr[i] = expr. */
/* Helper: is this token a compound assignment operator? */
static bool is_compound_assign(TokenType t) {
    return t == TOK_ASSIGN ||
           t == TOK_PLUS_EQ || t == TOK_MINUS_EQ || t == TOK_STAR_EQ ||
           t == TOK_SLASH_EQ || t == TOK_PERCENT_EQ || t == TOK_AMP_EQ ||
           t == TOK_PIPE_EQ || t == TOK_CARET_EQ || t == TOK_LSHIFT_EQ ||
           t == TOK_RSHIFT_EQ;
}

/* Map a compound assignment token to its binary operator symbol. */
static const char *compound_op(TokenType t) {
    switch (t) {
        case TOK_PLUS_EQ:    return "+";
        case TOK_MINUS_EQ:   return "-";
        case TOK_STAR_EQ:    return "*";
        case TOK_SLASH_EQ:   return "/";
        case TOK_PERCENT_EQ: return "%";
        case TOK_AMP_EQ:     return "&";
        case TOK_PIPE_EQ:    return "|";
        case TOK_CARET_EQ:   return "^";
        case TOK_LSHIFT_EQ:  return "<<";
        case TOK_RSHIFT_EQ:  return ">>";
        default:             return NULL;
    }
}

static ASTNode *parse_assignment_stmt(Parser *p) {
    int line = cur(p)->line;
    ASTNode *n = node_new(NODE_SET, line);
    n->sval = lu_strdup(cur(p)->value ? cur(p)->value : "_var");
    consume(p);

    while (check(p, TOK_DOT) || check(p, TOK_PTR_ACCESS) || check(p, TOK_LBRACKET)) {
        char extra[256] = "";
        if (check(p, TOK_DOT) || check(p, TOK_PTR_ACCESS)) {
            append_token_text(extra, sizeof(extra), check(p, TOK_PTR_ACCESS) ? "->" : ".");
            consume(p);
            append_token_text(extra, sizeof(extra), cur(p)->value ? cur(p)->value : "field");
            consume(p);
        } else {
            append_token_text(extra, sizeof(extra), "[");
            consume(p); /* [ */
            /* consume the full index expression: walk tokens until matching ] */
            int depth = 1;
            char idx[192] = "";
            while (depth > 0 && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
                if (check(p, TOK_LBRACKET)) depth++;
                else if (check(p, TOK_RBRACKET)) {
                    depth--;
                    if (depth == 0) break;
                }
                if (cur(p)->value) strncat(idx, cur(p)->value, sizeof(idx) - strlen(idx) - 1);
                consume(p);
            }
            append_token_text(extra, sizeof(extra), idx);
            append_token_text(extra, sizeof(extra), "]");
            match(p, TOK_RBRACKET);
        }
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s", n->sval ? n->sval : "", extra);
        free(n->sval);
        n->sval = lu_strdup(buf);
    }

    /* Match the assignment operator (plain or compound). */
    TokenType at = cur(p)->type;
    const char *cop = compound_op(at);
    if (is_compound_assign(at)) consume(p);
    else cop = NULL; /* fallback: no operator consumed; treat as plain = */

    ASTNode *rhs = parse_expr(p);

    if (cop) {
        /* x += y  →  x = x + y. Record the operator in op so codegen
           can expand it. */
        n->op = lu_strdup(cop);
    }
    node_list_add(&n->children, rhs);
    return n;
}

/* ─────────────────────────────────────────────
   Variable declaration
   (only when a type keyword is followed by ident = value)
   ───────────────────────────────────────────── */
static ASTNode *parse_var_decl(Parser *p) {
    int line = cur(p)->line;
    ASTNode *n = node_new(NODE_VAR_DECL, line);
    n->type_name = consume_type(p);
    n->sval = lu_strdup(cur(p)->value); consume(p); /* var name */
    /* optional [size] for arrays — supports multi-dimensional:
       int m[3][3], int cube[2][3][4]. We collect each dimension
       as a child; the first dimension is children[0], etc.
       For codegen we need to know how many dimensions there are,
       so we store the count in ival. */
    int ndims = 0;
    while (match(p, TOK_LBRACKET)) {
        if (ndims == 0) {
            n->op = lu_strdup("array");
        }
        node_list_add(&n->children, parse_expr(p));
        match(p, TOK_RBRACKET);
        ndims++;
    }
    n->ival = ndims;  /* number of dimensions (0 = not an array) */
    /* optional initialiser {a, b, c} */
    if (match(p, TOK_ASSIGN)) {
        if (match(p, TOK_LBRACE)) {
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                node_list_add(&n->children, parse_expr(p));
                match(p, TOK_COMMA);
            }
            match(p, TOK_RBRACE);
        } else {
            /* = expr (non-array initialiser) */
            node_list_add(&n->children, parse_expr(p));
        }
    }
    return n;
}

/* ─────────────────────────────────────────────
   MAIN parse_statement
   ───────────────────────────────────────────── */
static ASTNode *parse_statement(Parser *p) {
    skip_newlines(p);
    Token *t = cur(p);
    int line = t->line;

    switch (t->type) {
    /* ── L1 SYSTEM ── */
    case TOK_LANG_DECL: {
        ASTNode *n = node_new(NODE_LANG_DECL, line); consume(p); return n;
    }
    case TOK_IMPORT:
        return parse_import_like(p, NODE_IMPORT, line);
    case TOK_MODE: {
        consume(p);
        /* expect: us *mode* — consume rest of line */
        if (check(p, TOK_IDENT)) consume(p); /* 'us' */
        /* collect mode name: may be *word* (STAR IDENT STAR) or bare IDENT */
        char mode_buf[64] = "";
        if (check(p, TOK_STAR)) {
            consume(p); /* leading * */
            if (check(p, TOK_IDENT)) {
                strncpy(mode_buf, cur(p)->value, 63);
                consume(p);
            }
            if (check(p, TOK_STAR)) consume(p); /* trailing * */
        } else if (check(p, TOK_IDENT)) {
            strncpy(mode_buf, cur(p)->value, 63);
            consume(p);
        }
        ASTNode *n = node_new(NODE_MODE, line);
        n->sval = lu_strdup(mode_buf);
        /* consume any trailing tokens until newline */
        while (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) consume(p);
        return n;
    }
    case TOK_DEF_CONST: {
        consume(p);
        ASTNode *n = node_new(NODE_DEF_CONST, line);
        n->sval = lu_strdup(cur(p)->value); consume(p);
        match(p, TOK_ASSIGN);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_DEF_CONFIG: {
        consume(p);
        ASTNode *n = node_new(NODE_DEF_CONFIG, line);
        n->sval = lu_strdup(cur(p)->value); consume(p);
        match(p, TOK_ASSIGN);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_DEFINE: {
        consume(p);
        ASTNode *n = node_new(NODE_DEFINE, line);
        n->sval = lu_strdup(cur(p)->value); consume(p); /* name */
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_INCLUDE:
        return parse_import_like(p, NODE_INCLUDE, line);
    case TOK_LANG_BIND: {
        ASTNode *n = node_new(NODE_IMPORT, line);
        n->sval = lu_strdup(t->value);
        consume(p);
        while (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) consume(p);
        return n;
    }
    case TOK_OPT: {
        consume(p);
        ASTNode *n = node_new(NODE_OPT, line);
        n->sval = lu_strdup(cur(p)->value); consume(p); return n;
    }
    case TOK_ANNOT: {
        ASTNode *n = node_new(NODE_ANNOT, line);
        n->annot = lu_strdup(t->value); consume(p); return n;
    }

    /* ── L2 LOGIC ── */
    case TOK_BLOCK_ID: {
        ASTNode *n = node_new(NODE_BLOCK, line);
        n->block_n = atoi(t->value); consume(p);
        /* parse children until next #q or EOF */
        skip_newlines(p);
        while (!check(p, TOK_BLOCK_ID) && !check(p, TOK_BLOCK_END) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_BLOCK_ID) || check(p, TOK_BLOCK_END) || check(p, TOK_EOF)) break;
            int pos_before = p->pos;
            ASTNode *s = parse_statement(p);
            if (s) node_list_add(&n->children, s);
            opt_newline(p);
            if (p->pos == pos_before) consume(p); /* safety: never stall */
        }
        match(p, TOK_BLOCK_END);
        return n;
    }
    case TOK_IF:    return parse_if(p);
    case TOK_LOOP:  return parse_loop(p);
    case TOK_FUNC:  return parse_func(p, false);
    case TOK_TRY:   return parse_try(p);
    case TOK_STRUCT: {
        /* distinguish struct / class / interface / impl / template */
        const char *kw = cur(p)->value ? cur(p)->value : "";
        if (!strcmp(kw,"class") || !strcmp(kw,"interface") ||
            !strcmp(kw,"impl")  || !strcmp(kw,"template"))
            return parse_class(p);
        return parse_struct(p);
    }
    case TOK_UNION:  return parse_struct(p); /* unions parse like structs */
    case TOK_ENUM:   return parse_enum(p);
    case TOK_NS:     return parse_namespace(p);
    case TOK_MODULE: {
        consume(p);
        ASTNode *n = node_new(NODE_MODULE, line);
        n->sval = lu_strdup(cur(p)->value); consume(p); return n;
    }
    case TOK_PR: {
        consume(p);
        ASTNode *n = node_new(NODE_PR, line);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_TO: {
        consume(p);
        ASTNode *n = node_new(NODE_TO, line);
        node_list_add(&n->children, parse_statement(p)); return n;
    }
    case TOK_SET: {
        consume(p);
        ASTNode *n = node_new(NODE_SET, line);
        n->sval = lu_strdup(cur(p)->value); consume(p); /* var */
        /* optional field chain */
        while (check(p, TOK_DOT) || check(p, TOK_PTR_ACCESS) || check(p, TOK_LBRACKET)) {
            char extra[512] = "";
            if (check(p, TOK_DOT) || check(p, TOK_PTR_ACCESS)) {
                append_token_text(extra, sizeof(extra), check(p, TOK_PTR_ACCESS) ? "->" : ".");
                consume(p);
                append_token_text(extra, sizeof(extra), cur(p)->value ? cur(p)->value : "field");
                consume(p);
            } else {
                append_token_text(extra, sizeof(extra), "["); consume(p);
                /* consume the full index expression: walk tokens until matching ] */
                int depth = 1;
                char idx[256] = "";
                while (depth > 0 && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
                    if (check(p, TOK_LBRACKET)) depth++;
                    else if (check(p, TOK_RBRACKET)) {
                        depth--;
                        if (depth == 0) break;
                    }
                    if (cur(p)->value) strncat(idx, cur(p)->value, sizeof(idx) - strlen(idx) - 1);
                    consume(p);
                }
                append_token_text(extra, sizeof(extra), idx);
                append_token_text(extra, sizeof(extra), "]");
                match(p, TOK_RBRACKET);
            }
            char *old = n->sval;
            char buf[768]; snprintf(buf, sizeof(buf), "%s%s", old ? old : "", extra);
            free(old); n->sval = lu_strdup(buf);
        }
        /* Match assignment operator (plain or compound). */
        TokenType at = cur(p)->type;
        const char *cop = compound_op(at);
        if (is_compound_assign(at)) consume(p);
        else cop = NULL;
        ASTNode *rhs = parse_expr(p);
        if (cop) n->op = lu_strdup(cop);
        node_list_add(&n->children, rhs);
        return n;
    }
    case TOK_RETURN: {
        consume(p);
        ASTNode *n = node_new(NODE_RETURN, line);
        if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF))
            node_list_add(&n->children, parse_expr(p));
        return n;
    }
    case TOK_CALL: {
        consume(p);
        ASTNode *n = node_new(NODE_FUNC_CALL, line);
        n->sval = lu_strdup(cur(p)->value); consume(p);
        match(p, TOK_LPAREN);
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            node_list_add(&n->children, parse_expr(p));
            match(p, TOK_COMMA);
        }
        match(p, TOK_RPAREN); return n;
    }
    /* ── GAME ENGINE команды ── */
    case TOK_CR: {
        /* cr(obj;name) уже распарсен лексером — значение в t->value */
        ASTNode *n = node_new(NODE_CR, line);
        n->sval = lu_strdup(t->value); consume(p);
        /* optional exp после */
        if (check(p, TOK_EXP) || (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "exp")))
            consume(p);
        return n;
    }
    case TOK_DAM: {
        ASTNode *n = node_new(NODE_DAM, line);
        n->sval = lu_strdup(t->value); consume(p);
        /* optional (vl"-15") */
        if (check(p, TOK_SEMICOLON)) { consume(p); }
        if (check(p, TOK_LPAREN)) {
            consume(p);
            node_list_add(&n->children, parse_expr(p));
            match(p, TOK_RPAREN);
        }
        return n;
    }
    case TOK_ONL: {
        ASTNode *n = node_new(NODE_ONL, line);
        n->sval = lu_strdup(t->value); consume(p);
        /* optional следующее выражение */
        if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF))
            node_list_add(&n->children, parse_statement(p));
        return n;
    }
    case TOK_FUNC_CALL: {
        ASTNode *n = node_new(NODE_FUNC_CALL_GAME, line);
        n->sval = lu_strdup(t->value); consume(p);
        /* optional ;onl(...) */
        if (check(p, TOK_SEMICOLON)) { consume(p); }
        if (check(p, TOK_ONL)) {
            ASTNode *onl = node_new(NODE_ONL, line);
            onl->sval = lu_strdup(cur(p)->value); consume(p);
            node_list_add(&n->children, onl);
        }
        return n;
    }
    case TOK_EXP: {
        consume(p);
        return node_new(NODE_EXP, line);
    }
    case TOK_CL: {
        consume(p);
        ASTNode *n = node_new(NODE_CL, line);
        /* cl --#q3 → читаем номер блока */
        match(p, TOK_DEC); /* -- */
        if (check(p, TOK_BLOCK_ID)) {
            n->sval = lu_strdup(cur(p)->value); consume(p);
        }
        return n;
    }
    case TOK_LINREF: {
        ASTNode *n = node_new(NODE_LINREF, line);
        n->sval = lu_strdup(t->value); consume(p); return n;
    }
    case TOK_THROW: {
        consume(p);
        ASTNode *n = node_new(NODE_THROW, line);
        node_list_add(&n->children, parse_expr(p)); /* code or err const */
        if (check(p, TOK_STR_LIT)) {
            ASTNode *msg = node_new(NODE_LITERAL_STR, line);
            msg->sval = lu_strdup(cur(p)->value); consume(p);
            node_list_add(&n->children, msg);
        }
        return n;
    }

    /* ── L3 USER ── */
    case TOK_USER: {
        consume(p);
        ASTNode *n = node_new(NODE_USER_DECL, line);
        /* collect rest of line */
        char buf[512] = ""; int bi = 0;
        while (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
            if (cur(p)->value) {
                append_token_text(buf, sizeof(buf), cur(p)->value);
                bi = (int)strlen(buf);
                if (bi < (int)sizeof(buf) - 1) { buf[bi++] = ' '; buf[bi] = '\0'; }
            }
            consume(p);
        }
        n->sval = lu_strdup(buf); return n;
    }
    case TOK_SND_MES: {
        consume(p);
        ASTNode *n = node_new(NODE_SEND, line);
        if (check(p, TOK_LBRACKET)) {
            consume(p); /* eat [ */
            ASTNode *m = node_new(NODE_LITERAL_STR, line);
            m->sval = lu_strdup(cur(p)->value); consume(p); /* payload content */
            match(p, TOK_RBRACKET);
            node_list_add(&n->children, m);
        } else if (check(p, TOK_STR_LIT)) {
            ASTNode *m = node_new(NODE_LITERAL_STR, line);
            m->sval = lu_strdup(cur(p)->value); consume(p);
            node_list_add(&n->children, m);
        } else if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
            node_list_add(&n->children, parse_expr(p));
        }
        return n;
    }
    case TOK_REC: {
        consume(p);
        ASTNode *n = node_new(NODE_RECV, line);
        if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF))
            node_list_add(&n->children, parse_expr(p));
        return n;
    }
    case TOK_FWD: {
        consume(p);
        ASTNode *n = node_new(NODE_SEND, line);
        n->sval = lu_strdup("fwd");
        if (!check(p, TOK_NEWLINE)) node_list_add(&n->children, parse_expr(p));
        return n;
    }
    case TOK_DEL: {
        consume(p);
        ASTNode *n = node_new(NODE_FUNC_CALL, line);
        n->sval = lu_strdup("del_mes");
        node_list_add(&n->children, parse_expr(p)); return n;
    }

    /* ── L4 NETWORK ── */
    case TOK_SERVER: {
        consume(p);
        ASTNode *n = node_new(NODE_SERVER_DECL, line);
        /* read #q[n] */
        if (check(p, TOK_BLOCK_ID)) { n->block_n = atoi(cur(p)->value); consume(p); }
        /* rest of line as sval (bounded) */
        char buf[512] = "";
        while (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
            if (cur(p)->value) {
                append_token_text(buf, sizeof(buf), cur(p)->value);
                append_token_text(buf, sizeof(buf), " ");
            }
            consume(p);
        }
        n->sval = lu_strdup(buf); return n;
    }
    case TOK_COR: {
        ASTNode *n = node_new(NODE_LITERAL_COR, line);
        n->sval = lu_strdup(t->value); consume(p); return n;
    }
    case TOK_INQ: {
        consume(p);
        ASTNode *n = node_new(NODE_INQ, line);
        /* expect: mes#q[n] or data#q[n] etc. */
        n->sval = lu_strdup(cur(p)->value); consume(p); return n;
    }
    case TOK_SEND: {
        ASTNode *n = node_new(NODE_SEND, line);
        n->sval = lu_strdup(t->value); consume(p);
        /* optional → ip ... */
        if (match(p, TOK_ARROW)) {
            if (check(p, TOK_TYPE_IP)) consume(p);
            ASTNode *ip = node_new(NODE_LITERAL_IP, line);
            ip->sval = lu_strdup(cur(p)->value); consume(p);
            node_list_add(&n->children, ip);
        }
        return n;
    }
    case TOK_BCAST: {
        ASTNode *n = node_new(NODE_BCAST, line);
        n->sval = lu_strdup(t->value); consume(p); return n;
    }
    case TOK_ROUTE: {
        consume(p);
        ASTNode *n = node_new(NODE_ROUTE, line);
        node_list_add(&n->children, parse_expr(p));
        if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "via")) consume(p);
        node_list_add(&n->children, parse_expr(p)); return n;
    }

    /* ── C-LEVEL MEMORY ── */
    case TOK_PTR: {
        consume(p);
        ASTNode *n = node_new(NODE_PTR_DECL, line);
        n->type_name = consume_type(p);
        n->sval = lu_strdup(cur(p)->value); consume(p);
        if (match(p, TOK_ASSIGN)) node_list_add(&n->children, parse_expr(p));
        return n;
    }
    case TOK_ALLOC: {
        consume(p);
        ASTNode *n = node_new(NODE_ALLOC, line);
        /* type:size */
        n->type_name = consume_type(p);
        match(p, TOK_COLON);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_FREE: {
        consume(p);
        ASTNode *n = node_new(NODE_FREE, line);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_MEMSET: {
        consume(p);
        ASTNode *n = node_new(NODE_MEMSET, line);
        for (int i = 0; i < 3 && !check(p, TOK_NEWLINE) && !check(p, TOK_EOF); i++) {
            node_list_add(&n->children, parse_expr(p));
        }
        return n;
    }
    case TOK_MEMCPY: {
        consume(p);
        ASTNode *n = node_new(NODE_MEMCPY, line);
        for (int i = 0; i < 3 && !check(p, TOK_NEWLINE) && !check(p, TOK_EOF); i++) {
            node_list_add(&n->children, parse_expr(p));
        }
        return n;
    }
    case TOK_REF: {
        consume(p);
        ASTNode *n = node_new(NODE_EXPR_REF, line);
        ASTNode *inner = node_new(NODE_IDENT, line);
        inner->sval = lu_strdup(cur(p)->value); consume(p);
        node_list_add(&n->children, inner); return n;
    }
    case TOK_DEREF: {
        consume(p);
        ASTNode *n = node_new(NODE_EXPR_DEREF, line);
        ASTNode *inner = node_new(NODE_IDENT, line);
        inner->sval = lu_strdup(cur(p)->value); consume(p);
        node_list_add(&n->children, inner); return n;
    }

    /* ── ASYNC ── */
    case TOK_AWAIT: {
        consume(p);
        ASTNode *n = node_new(NODE_AWAIT, line);
        node_list_add(&n->children, parse_statement(p)); return n;
    }
    case TOK_SPAWN: {
        consume(p);
        ASTNode *n = node_new(NODE_SPAWN, line);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_CHAN: {
        consume(p); /* eat Chan/ */
        ASTNode *n = node_new(NODE_CHAN_DECL, line);
        /* Chan/name — there is no type, the name IS the variable.
           Old code called consume_type() which ate the name as a type,
           producing `lu_chan_t * = lu_chan_new();` (missing var name).
           Channels are always lu_chan_t*, so we hardcode the type. */
        n->type_name = lu_strdup("chan");
        n->sval = lu_strdup(cur(p)->value ? cur(p)->value : "_chan");
        consume(p); /* eat the channel name */
        return n;
    }
    case TOK_CHAN_SEND: {
        consume(p);
        ASTNode *n = node_new(NODE_CHAN_SEND, line);
        n->sval = lu_strdup(cur(p)->value); consume(p); /* channel */
        match(p, TOK_LARROW);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_CHAN_RECV: {
        consume(p);
        ASTNode *n = node_new(NODE_CHAN_RECV, line);
        n->sval = lu_strdup(cur(p)->value); consume(p); return n;
    }
    case TOK_EVENT: {
        consume(p);
        ASTNode *n = node_new(NODE_EVENT, line);
        n->sval = lu_strdup(cur(p)->value); consume(p); return n;
    }
    case TOK_EMIT: {
        consume(p);
        ASTNode *n = node_new(NODE_EMIT, line);
        n->sval = lu_strdup(cur(p)->value); consume(p);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_ON: {
        consume(p);
        ASTNode *n = node_new(NODE_ON, line);
        n->sval = lu_strdup(cur(p)->value); consume(p);
        match(p, TOK_ARROW);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_SELECT: {
        consume(p);
        ASTNode *n = node_new(NODE_SELECT, line);
        skip_newlines(p); match(p, TOK_LBRACE); skip_newlines(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_RBRACE)) break;
            node_list_add(&n->children, parse_statement(p));
            opt_newline(p);
        }
        match(p, TOK_RBRACE); return n;
    }

    /* ── DEBUG ── */
    case TOK_LOG: {
        consume(p);
        ASTNode *n = node_new(NODE_LOG, line);
        n->sval = lu_strdup(cur(p)->value); /* level */ consume(p);
        node_list_add(&n->children, parse_expr(p)); return n;
    }
    case TOK_ASSERT: {
        consume(p);
        ASTNode *n = node_new(NODE_ASSERT, line);
        node_list_add(&n->children, parse_expr(p));
        if (check(p, TOK_STR_LIT)) {
            ASTNode *msg = node_new(NODE_LITERAL_STR, line);
            msg->sval = lu_strdup(cur(p)->value); consume(p);
            node_list_add(&n->children, msg);
        }
        return n;
    }
    case TOK_TRACE: {
        consume(p);
        ASTNode *n = node_new(NODE_FUNC_CALL, line);
        n->sval = lu_strdup("__lu_trace");
        n->type_name = lu_strdup(cur(p)->value); consume(p);
        return n;
    }
    case TOK_BREAK_BP: {
        consume(p);
        /* Break/ = C break statement (loop exit) */
        ASTNode *n = node_new(NODE_RETURN, line);
        n->sval = lu_strdup("__break__");
        return n;
    }
    case TOK_WATCH: {
        consume(p);
        ASTNode *n = node_new(NODE_FUNC_CALL, line);
        n->sval = lu_strdup("__lu_watch");
        n->type_name = lu_strdup(cur(p)->value); consume(p); return n;
    }

    /* ── Type keywords starting a var decl ── */
    case TOK_TYPE_INT: case TOK_TYPE_INT64: case TOK_TYPE_FLOAT:
    case TOK_TYPE_STR: case TOK_TYPE_BOOL: case TOK_TYPE_BYTE:
    case TOK_TYPE_IP: case TOK_TYPE_ID: case TOK_TYPE_MSG:
    case TOK_TYPE_LIB: case TOK_TYPE_VOID: case TOK_TYPE_COR:
        return parse_var_decl(p);

    /* ?T and !T — optional and error union types (Zig-style) */
    case TOK_QUESTION:
    case TOK_BANG:
        return parse_var_decl(p);

    /* ── IDENT — could be type, assignment, or expression ── */
    case TOK_IDENT: {
        const char *v = cur(p)->value ? cur(p)->value : "";

        /* Vector<T> name — built-in generic container type declaration.
           Must be checked before the assignment/expression fallback,
           because `Vector < int > nums` looks like a comparison expression. */
        if (!strcmp(v, "Vector") && peek_tok(p, 1)->type == TOK_LT) {
            return parse_var_decl(p);
        }
        /* Unique<T> and Shared<T> — smart pointer types */
        if ((!strcmp(v, "Unique") || !strcmp(v, "Shared")) &&
            peek_tok(p, 1)->type == TOK_LT) {
            return parse_var_decl(p);
        }
        /* fn(args) -> ret name — function pointer type declaration.
           Must be checked before the expression fallback because
           `fn(int, int) -> int op = add` looks like a function call. */
        if (!strcmp(v, "fn") && peek_tok(p, 1)->type == TOK_LPAREN) {
            return parse_var_decl(p);
        }

        /* ── Python-style keywords (hybrid mode: both styles work) ── */

        /* if cond { } — Python/C-style if (alternative to If/) */
        if (!strcmp(v, "if")) {
            consume(p);
            ASTNode *n = node_new(NODE_IF, line);
            node_list_add(&n->children, parse_expr(p));
            skip_newlines(p);
            ASTNode *then_body = node_new(NODE_PROGRAM, line);
            if (match(p, TOK_LBRACE)) {
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    skip_newlines(p);
                    if (check(p, TOK_RBRACE)) break;
                    int pos_before = p->pos;
                    ASTNode *s = parse_statement(p);
                    if (s) node_list_add(&then_body->children, s);
                    opt_newline(p);
                    if (p->pos == pos_before) consume(p);
                }
                match(p, TOK_RBRACE);
            }
            node_list_add(&n->children, then_body);
            /* check for elif/else chain */
            skip_newlines(p);
            if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "elif")) {
                ASTNode *alt = parse_statement(p);
                if (alt) node_list_add(&n->children, alt);
            } else if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "else")) {
                consume(p);
                skip_newlines(p);
                ASTNode *else_body = node_new(NODE_PROGRAM, line);
                if (match(p, TOK_LBRACE)) {
                    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                        skip_newlines(p);
                        if (check(p, TOK_RBRACE)) break;
                        int pos_before = p->pos;
                        ASTNode *s = parse_statement(p);
                        if (s) node_list_add(&else_body->children, s);
                        opt_newline(p);
                        if (p->pos == pos_before) consume(p);
                    }
                    match(p, TOK_RBRACE);
                }
                node_list_add(&n->children, else_body);
            }
            return n;
        }

        /* def name(args):Type { } or def name(args) -> Type { } — function declaration */
        if (!strcmp(v, "def")) {
            consume(p); /* eat 'def' */
            /* Temporarily transform into Fn/ parsing by reading name and calling parse_func-like logic */
            ASTNode *n = node_new(NODE_FUNC_DECL, line);
            if (check(p, TOK_IDENT)) { n->sval = lu_strdup(cur(p)->value); consume(p); }
            match(p, TOK_LPAREN);
            parse_params(p, n);
            match(p, TOK_RPAREN);
            /* return type: :Type or -> Type (-> is TOK_PTR_ACCESS in our lexer) */
            if (match(p, TOK_COLON)) n->type_name = consume_type(p);
            else if (match(p, TOK_PTR_ACCESS)) n->type_name = consume_type(p);
            else if (match(p, TOK_ARROW)) n->type_name = consume_type(p);
            skip_newlines(p);
            if (match(p, TOK_LBRACE)) {
                ASTNode *body = node_new(NODE_PROGRAM, line);
                skip_newlines(p);
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    skip_newlines(p);
                    if (check(p, TOK_RBRACE)) break;
                    int pos_before = p->pos;
                    ASTNode *s = parse_statement(p);
                    if (s) node_list_add(&body->children, s);
                    opt_newline(p);
                    if (p->pos == pos_before) consume(p);
                }
                match(p, TOK_RBRACE);
                node_list_add(&n->children, body);
            }
            return n;
        }

        /* print expr  or  print(expr)  — Python-style print */
        if (!strcmp(v, "print")) {
            /* Only treat as print keyword if followed by ( or a non-assignment token */
            TokenType next = peek_tok(p, 1)->type;
            if (next != TOK_ASSIGN && next != TOK_DOT && next != TOK_LBRACKET &&
                !is_compound_assign(next)) {
                consume(p); /* eat 'print' */
                ASTNode *n = node_new(NODE_PR, line);
                if (check(p, TOK_LPAREN)) {
                    consume(p);
                    if (!check(p, TOK_RPAREN) && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
                        node_list_add(&n->children, parse_expr(p));
                        while (match(p, TOK_COMMA)) {
                            node_list_add(&n->children, parse_expr(p));
                        }
                    }
                    match(p, TOK_RPAREN);
                } else if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
                    node_list_add(&n->children, parse_expr(p));
                }
                return n;
            }
        }

        /* return expr  or  return — Python/C-style return */
        if (!strcmp(v, "return")) {
            consume(p);
            ASTNode *n = node_new(NODE_RETURN, line);
            if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF))
                node_list_add(&n->children, parse_expr(p));
            return n;
        }

        /* auto name = expr — type inference */
        if (!strcmp(v, "auto")) {
            consume(p);
            ASTNode *n = node_new(NODE_VAR_DECL, line);
            n->type_name = lu_strdup("auto");
            if (check(p, TOK_IDENT)) { n->sval = lu_strdup(cur(p)->value); consume(p); }
            /* optional template <T> */
            if (match(p, TOK_LT)) {
                char tbuf[128]; snprintf(tbuf, sizeof(tbuf), "%s", cur(p)->value ? cur(p)->value : "T");
                consume(p);
                match(p, TOK_GT);
                n->annot = lu_strdup(tbuf);
            }
            match(p, TOK_ASSIGN);
            if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF))
                node_list_add(&n->children, parse_expr(p));
            return n;
        }

        /* while cond { } — Python/C-style while loop */
        if (!strcmp(v, "while")) {
            consume(p);
            ASTNode *n = node_new(NODE_LOOP_WHILE, line);
            node_list_add(&n->children, parse_expr(p));
            skip_newlines(p);
            if (match(p, TOK_LBRACE)) {
                ASTNode *body = node_new(NODE_PROGRAM, line);
                skip_newlines(p);
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    skip_newlines(p);
                    if (check(p, TOK_RBRACE)) break;
                    int pos_before = p->pos;
                    ASTNode *s = parse_statement(p);
                    if (s) node_list_add(&body->children, s);
                    opt_newline(p);
                    if (p->pos == pos_before) consume(p);
                }
                match(p, TOK_RBRACE);
                node_list_add(&n->children, body);
            }
            return n;
        }

        /* for x in expr { } — Python-style for-each loop */
        if (!strcmp(v, "for")) {
            consume(p);
            ASTNode *n = node_new(NODE_LOOP_EACH, line);
            /* item name */
            ASTNode *item = node_new(NODE_IDENT, line);
            if (check(p, TOK_IDENT)) { item->sval = lu_strdup(cur(p)->value); consume(p); }
            node_list_add(&n->children, item);
            /* expect 'in' */
            if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "in")) consume(p);
            node_list_add(&n->children, parse_expr(p));
            /* body */
            skip_newlines(p);
            if (match(p, TOK_LBRACE)) {
                ASTNode *body = node_new(NODE_PROGRAM, line);
                skip_newlines(p);
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    skip_newlines(p);
                    if (check(p, TOK_RBRACE)) break;
                    int pos_before = p->pos;
                    ASTNode *s = parse_statement(p);
                    if (s) node_list_add(&body->children, s);
                    opt_newline(p);
                    if (p->pos == pos_before) consume(p);
                }
                match(p, TOK_RBRACE);
                node_list_add(&n->children, body);
            }
            return n;
        }

        /* break — loop exit */
        if (!strcmp(v, "break")) {
            consume(p);
            ASTNode *n = node_new(NODE_RETURN, line);
            n->sval = lu_strdup("__break__");
            return n;
        }

        /* continue — loop skip */
        if (!strcmp(v, "continue")) {
            consume(p);
            ASTNode *n = node_new(NODE_RETURN, line);
            n->sval = lu_strdup("__continue__");
            return n;
        }

        /* defer statement — runs at scope exit.
           Syntax: defer <expression statement>
           Example: defer free(p)
           Lowered to a cleanup-attribute variable that calls the
           deferred expression at scope exit. */
        if (!strcmp(v, "defer")) {
            consume(p); /* eat 'defer' */
            ASTNode *n = node_new(NODE_DEFER, line);
            /* Parse the deferred statement (typically a function call). */
            ASTNode *stmt = parse_statement(p);
            if (stmt) node_list_add(&n->children, stmt);
            return n;
        }

        /* match expr { case val { ... } case _ { ... } } — pattern matching */
        if (!strcmp(v, "match")) {
            consume(p); /* eat 'match' */
            ASTNode *n = node_new(NODE_MATCH, line);
            /* The expression to match */
            node_list_add(&n->children, parse_expr(p));
            skip_newlines(p);
            /* Expect { */
            if (!match(p, TOK_LBRACE)) return n;
            skip_newlines(p);
            /* Parse cases */
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                skip_newlines(p);
                if (check(p, TOK_RBRACE)) break;
                int pos_before = p->pos;
                /* Expect 'case' */
                if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "case")) {
                    consume(p); /* eat 'case' */
                    ASTNode *case_node = node_new(NODE_PROGRAM, cur(p)->line);
                    /* The case value: either '_' (default) or an expression */
                    if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "_")) {
                        consume(p); /* eat '_' */
                        case_node->annot = lu_strdup("default");
                    } else {
                        case_node->annot = lu_strdup("case");
                        node_list_add(&case_node->children, parse_expr(p));
                    }
                    skip_newlines(p);
                    /* Body { } */
                    if (match(p, TOK_LBRACE)) {
                        ASTNode *body = node_new(NODE_PROGRAM, cur(p)->line);
                        skip_newlines(p);
                        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                            skip_newlines(p);
                            if (check(p, TOK_RBRACE)) break;
                            int body_pos = p->pos;
                            ASTNode *s = parse_statement(p);
                            if (s) node_list_add(&body->children, s);
                            opt_newline(p);
                            if (p->pos == body_pos) consume(p);
                        }
                        match(p, TOK_RBRACE);
                        node_list_add(&case_node->children, body);
                    }
                    node_list_add(&n->children, case_node);
                } else {
                    /* Not a 'case' keyword — skip to avoid infinite loop */
                    consume(p);
                }
                opt_newline(p);
                if (p->pos == pos_before) consume(p);
            }
            match(p, TOK_RBRACE);
            return n;
        }

        /* elif cond { } — Python-style else-if (must be checked before generic ident) */
        if (!strcmp(v, "elif")) {
            /* Treat like If/ — parse_if expects to consume If/ or Elif/ */
            consume(p);
            /* Build an if node manually */
            ASTNode *n = node_new(NODE_IF, line);
            node_list_add(&n->children, parse_expr(p));
            skip_newlines(p);
            ASTNode *then_body = node_new(NODE_PROGRAM, line);
            if (match(p, TOK_LBRACE)) {
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    skip_newlines(p);
                    if (check(p, TOK_RBRACE)) break;
                    int pos_before = p->pos;
                    ASTNode *s = parse_statement(p);
                    if (s) node_list_add(&then_body->children, s);
                    opt_newline(p);
                    if (p->pos == pos_before) consume(p);
                }
                match(p, TOK_RBRACE);
            }
            node_list_add(&n->children, then_body);
            /* check for elif/else chain */
            skip_newlines(p);
            if (check(p, TOK_IDENT) && (!strcmp(cur(p)->value, "elif"))) {
                ASTNode *alt = parse_statement(p);
                if (alt) node_list_add(&n->children, alt);
            } else if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "else")) {
                consume(p);
                skip_newlines(p);
                ASTNode *else_body = node_new(NODE_PROGRAM, line);
                if (match(p, TOK_LBRACE)) {
                    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                        skip_newlines(p);
                        if (check(p, TOK_RBRACE)) break;
                        int pos_before = p->pos;
                        ASTNode *s = parse_statement(p);
                        if (s) node_list_add(&else_body->children, s);
                        opt_newline(p);
                        if (p->pos == pos_before) consume(p);
                    }
                    match(p, TOK_RBRACE);
                }
                node_list_add(&n->children, else_body);
            }
            return n;
        }

        /* else { } — Python-style else (standalone, usually after if) */
        if (!strcmp(v, "else")) {
            consume(p);
            skip_newlines(p);
            ASTNode *else_body = node_new(NODE_PROGRAM, line);
            if (match(p, TOK_LBRACE)) {
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    skip_newlines(p);
                    if (check(p, TOK_RBRACE)) break;
                    int pos_before = p->pos;
                    ASTNode *s = parse_statement(p);
                    if (s) node_list_add(&else_body->children, s);
                    opt_newline(p);
                    if (p->pos == pos_before) consume(p);
                }
                match(p, TOK_RBRACE);
            }
            return else_body;
        }

        /* chan <- value — channel send (Python/Go-style with arrow).
           This is the documented syntax in the README; the legacy
           `Send/chan <- value` form is also still supported via TOK_CHAN_SEND. */
        if (peek_tok(p, 1)->type == TOK_LARROW) {
            consume(p); /* eat channel name */
            consume(p); /* eat <- */
            ASTNode *n = node_new(NODE_CHAN_SEND, line);
            n->sval = lu_strdup(v);
            node_list_add(&n->children, parse_expr(p));
            return n;
        }

        /* C-style assignment: x = expr, obj.field = expr, arr[i] = expr,
           and compound forms: x += expr, arr[i] <<= expr, etc. */
        if (is_compound_assign(peek_tok(p, 1)->type) ||
            peek_tok(p, 1)->type == TOK_DOT ||
            peek_tok(p, 1)->type == TOK_PTR_ACCESS ||
            peek_tok(p, 1)->type == TOK_LBRACKET) {
            int scan = p->pos + 1;
            bool is_assign = false;
            while (scan < p->count && p->tokens[scan].type != TOK_NEWLINE &&
                   p->tokens[scan].type != TOK_EOF) {
                if (is_compound_assign(p->tokens[scan].type)) { is_assign = true; break; }
                if (p->tokens[scan].type == TOK_LPAREN) break;
                scan++;
            }
            if (is_assign) return parse_assignment_stmt(p);
        }

        /* class / interface / impl / template as IDENT (lexer maps to TOK_IDENT for these) */
        if (!strcmp(v,"class") || !strcmp(v,"interface") ||
            !strcmp(v,"impl")  || !strcmp(v,"template"))
            return parse_class(p);

        /* new ClassName(...) — heap allocation */
        if (!strcmp(v, "new")) {
            consume(p);
            ASTNode *n = node_new(NODE_NEW_EXPR, line);
            n->sval = lu_strdup(cur(p)->value); consume(p); /* type name */
            /* optional template arg <T> */
            if (check(p, TOK_LT)) {
                consume(p);
                n->annot = lu_strdup(cur(p)->value); consume(p);
                match(p, TOK_GT);
            }
            match(p, TOK_LPAREN);
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                node_list_add(&n->children, parse_expr(p));
                match(p, TOK_COMMA);
            }
            match(p, TOK_RPAREN);
            return n;
        }

        /* delete varname */
        if (!strcmp(v, "delete")) {
            consume(p);
            ASTNode *n = node_new(NODE_DELETE_EXPR, line);
            n->sval = lu_strdup(cur(p)->value); consume(p);
            return n;
        }

        /* obj;name.(attrs) — object declaration */
        if (!strcmp(v, "obj")) {
            consume(p);
            ASTNode *n = node_new(NODE_OBJ_DECL, line);
            match(p, TOK_SEMICOLON);
            n->sval = lu_strdup(cur(p)->value); consume(p); /* name */
            if (check(p, TOK_DOT)) {
                consume(p);
                match(p, TOK_LPAREN);
                char attrs[256] = {0};
                while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                    strncat(attrs, cur(p)->value, sizeof(attrs)-strlen(attrs)-1);
                    consume(p);
                }
                match(p, TOK_RPAREN);
                n->type_name = lu_strdup(attrs);
            }
            return n;
        }

        /* exp — standalone spawn/activate command */
        if (!strcmp(v, "exp")) {
            consume(p);
            return node_new(NODE_EXP, line);
        }

        /* cl --#qN / cl(target) — block call or object select */
        if (!strcmp(v, "cl")) {
            consume(p);
            if (match(p, TOK_DEC)) {
                ASTNode *n = node_new(NODE_CL, line);
                if (check(p, TOK_BLOCK_ID)) {
                    n->sval = lu_strdup(cur(p)->value);
                    consume(p);
                }
                return n;
            }
            ASTNode *n = node_new(NODE_CL_CALL, line);
            match(p, TOK_LPAREN);
            n->sval = lu_strdup(cur(p)->value); consume(p);
            match(p, TOK_RPAREN);
            return n;
        }

        /* dam(value) — damage call */
        if (!strcmp(v, "dam")) {
            consume(p);
            ASTNode *n = node_new(NODE_DAM_CALL, line);
            match(p, TOK_LPAREN);
            node_list_add(&n->children, parse_expr(p));
            match(p, TOK_RPAREN);
            return n;
        }

        /* onl(snd --obj; #qN) — targeted conditional send */
        if (!strcmp(v, "onl")) {
            consume(p);
            ASTNode *n = node_new(NODE_ONL_CALL, line);
            match(p, TOK_LPAREN);
            /* parse inner: snd --obj; #qN */
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                if (check(p, TOK_IDENT) && !strcmp(cur(p)->value, "snd")) {
                    consume(p); /* snd */
                    /* -- */
                    match(p, TOK_DEC);
                    /* obj identifier */
                    ASTNode *target = node_new(NODE_IDENT, line);
                    target->sval = lu_strdup(cur(p)->value); consume(p);
                    node_list_add(&n->children, target);
                    match(p, TOK_SEMICOLON);
                } else if (check(p, TOK_BLOCK_ID)) {
                    ASTNode *qref = node_new(NODE_IDENT, line);
                    char buf[32]; snprintf(buf, sizeof(buf), "_q%s", cur(p)->value);
                    qref->sval = lu_strdup(buf); consume(p);
                    node_list_add(&n->children, qref);
                } else {
                    consume(p);
                }
            }
            match(p, TOK_RPAREN);
            return n;
        }

        /* snd --#qN — conditional goto */
        if (!strcmp(v, "snd")) {
            consume(p);
            ASTNode *n = node_new(NODE_SND_GOTO, line);
            match(p, TOK_DEC); /* -- */
            if (check(p, TOK_BLOCK_ID)) {
                char buf[32]; snprintf(buf, sizeof(buf), "_q%s", cur(p)->value);
                n->sval = lu_strdup(buf); consume(p);
            } else if (check(p, TOK_IDENT)) {
                n->sval = lu_strdup(cur(p)->value); consume(p);
            }
            return n;
        }

        /* this.field */
        if (!strcmp(v, "this") || !strcmp(v, "super")) {
            ASTNode *n = node_new(NODE_THIS_EXPR, line);
            n->sval = lu_strdup(v); consume(p);
            return n;
        }

        /* look ahead: if next non-ws is IDENT → var decl with user-defined type */
        if (is_type_tok(peek_tok(p, 1)->type) || peek_tok(p, 1)->type == TOK_IDENT) {
            /* possible user-type var decl: MyType varname = ... */
            return parse_var_decl(p);
        }
        /* otherwise expression statement. Bare function calls are statements;
           explicit Pr/foo() is still parsed as NODE_PR and printed. */
        ASTNode *expr = parse_expr(p);
        if (expr && expr->kind == NODE_FUNC_CALL) return expr;
        ASTNode *n = node_new(NODE_PR, line);
        node_list_add(&n->children, expr); return n;
    }

    default:
        /* skip unknowns */
        consume(p);
        return NULL;
    }
}


/* ─────────────────────────────────────────────
   ENTRY POINT
   ───────────────────────────────────────────── */
ASTNode *parse(Token *tokens, int count) {
    Parser p = { tokens, count, 0 };
    ASTNode *root = node_new(NODE_PROGRAM, 0);
    while (!check(&p, TOK_EOF)) {
        skip_newlines(&p);
        if (check(&p, TOK_EOF)) break;
        int pos_before = p.pos;
        ASTNode *s = parse_statement(&p);
        if (s) node_list_add(&root->children, s);
        opt_newline(&p);
        if (p.pos == pos_before) consume(&p); /* safety: never stall */
    }
    return root;
}