#include "lu.h"

/* ─────────────────────────────────────────────
   Symbol table helpers
   ───────────────────────────────────────────── */
static void sym_add(SymTable *st, const char *name, const char *type,
                    int scope, bool is_ptr, bool is_func,
                    bool is_const, int decl_line) {
    if (st->count >= SYM_MAX) {
        lu_warn(decl_line, "symbol table full, skipping '%s'", name ? name : "?");
        return;
    }
    Symbol *s = &st->entries[st->count++];
    strncpy(s->name, name ? name : "?", sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    strncpy(s->type, type ? type : "auto", sizeof(s->type) - 1);
    s->type[sizeof(s->type) - 1] = '\0';
    s->block_scope = scope;
    s->is_ptr      = is_ptr;
    s->is_func     = is_func;
    s->is_const    = is_const;
    s->decl_line   = decl_line;
}

static Symbol *sym_find(SymTable *st, const char *name) {
    for (int i = st->count - 1; i >= 0; i--)
        if (!strcmp(st->entries[i].name, name)) return &st->entries[i];
    return NULL;
}

/* Find in same scope only (for duplicate detection) */
static Symbol *sym_find_in_scope(SymTable *st, const char *name, int scope) {
    for (int i = 0; i < st->count; i++)
        if (st->entries[i].block_scope == scope &&
            !strcmp(st->entries[i].name, name))
            return &st->entries[i];
    return NULL;
}

static bool semantic_is_class_like(NodeKind k) {
    return k == NODE_CLASS_DECL || k == NODE_INTERFACE_DECL ||
           k == NODE_IMPL_DECL || k == NODE_TEMPLATE_DECL;
}

static bool semantic_is_method_like(NodeKind k) {
    return k == NODE_METHOD_DECL || k == NODE_CONSTRUCTOR ||
           k == NODE_DESTRUCTOR || k == NODE_OP_OVERLOAD;
}

static const char *semantic_op_mangle(const char *op) {
    if (!op) return "op";
    if (!strcmp(op, "+")) return "add";
    if (!strcmp(op, "-")) return "sub";
    if (!strcmp(op, "*")) return "mul";
    if (!strcmp(op, "/")) return "div";
    if (!strcmp(op, "%")) return "mod";
    if (!strcmp(op, "==")) return "eq";
    if (!strcmp(op, "!=")) return "neq";
    if (!strcmp(op, "<")) return "lt";
    if (!strcmp(op, ">")) return "gt";
    if (!strcmp(op, "<=")) return "le";
    if (!strcmp(op, ">=")) return "ge";
    return "op";
}

/* ─────────────────────────────────────────────
   Type compatibility check
   ───────────────────────────────────────────── */
static bool types_compatible(const char *a, const char *b) {
    if (!a || !b) return true;          /* unknown → allow */
    if (!strcmp(a, b)) return true;
    /* numeric widening */
    if ((!strcmp(a,"int")   && !strcmp(b,"int64")) ||
        (!strcmp(a,"int64") && !strcmp(b,"int"))   ||
        (!strcmp(a,"int")   && !strcmp(b,"float")) ||
        (!strcmp(a,"float") && !strcmp(b,"int"))   ||
        (!strcmp(a,"byte")  && !strcmp(b,"int"))   ||
        (!strcmp(a,"int")   && !strcmp(b,"byte")))
        return true;
    /* id and str are both char* under the hood */
    if ((!strcmp(a,"id") && !strcmp(b,"str")) ||
        (!strcmp(a,"str") && !strcmp(b,"id")))
        return true;
    /* Low-level helpers often return a C pointer used as Lu str/file handle. */
    if ((!strcmp(a,"str") && !strcmp(b,"ptr")) ||
        (!strcmp(a,"id")  && !strcmp(b,"ptr")) ||
        (!strcmp(a,"ptr") && !strcmp(b,"str")) ||
        (!strcmp(a,"ptr") && !strcmp(b,"id")))
        return true;
    return false;
}

/* Infer the Lu type of an expression node */
static const char *infer_type(SymTable *st, ASTNode *n) {
    if (!n) return "void";
    switch (n->kind) {
    case NODE_LITERAL_INT:   return "int";
    case NODE_LITERAL_FLOAT: return "float";
    case NODE_LITERAL_STR:   return "str";
    case NODE_LITERAL_BOOL:  return "bool";
    case NODE_LITERAL_IP:    return "ip";
    case NODE_LITERAL_COR:   return "cor";
    case NODE_IDENT: {
        Symbol *s = sym_find(st, n->sval ? n->sval : "");
        return s ? s->type : "auto";
    }
    case NODE_EXPR_BINOP: {
        const char *lt = infer_type(st, n->children.count > 0 ? n->children.items[0] : NULL);
        const char *rt = infer_type(st, n->children.count > 1 ? n->children.items[1] : NULL);
        /* comparison → bool */
        const char *op = n->op ? n->op : "";
        if (!strcmp(op,"==") || !strcmp(op,"!=") || !strcmp(op,"<") ||
            !strcmp(op,">")  || !strcmp(op,"<=") || !strcmp(op,">=") ||
            !strcmp(op,"&&") || !strcmp(op,"||"))
            return "bool";
        /* prefer wider numeric */
        if (!strcmp(lt,"float") || !strcmp(rt,"float")) return "float";
        if (!strcmp(lt,"int64") || !strcmp(rt,"int64")) return "int64";
        return lt;
    }
    case NODE_EXPR_UNOP:
        return infer_type(st, n->children.count > 0 ? n->children.items[0] : NULL);
    case NODE_FUNC_CALL: {
        Symbol *s = sym_find(st, n->sval ? n->sval : "");
        return s ? s->type : "auto";
    }
    default: return "auto";
    }
}

/* ─────────────────────────────────────────────
   Cor key validation
   ───────────────────────────────────────────── */
static bool validate_cor_key(const char *key, int line) {
    if (!key || !*key) {
        lu_warn(line, "empty cor/ key");
        return false;
    }
    /* key must be comma-separated integers */
    char buf[512];
    strncpy(buf, key, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, ",");
    int count = 0;
    while (tok) {
        while (*tok == ' ') tok++;
        char *end;
        strtol(tok, &end, 10);
        while (*end == ' ') end++;
        if (*end != '\0') {
            lu_warn(line, "cor/ key contains non-integer value '%s'", tok);
            return false;
        }
        count++;
        tok = strtok(NULL, ",");
    }
    if (count < 1) {
        lu_warn(line, "cor/ key has no values");
        return false;
    }
    return true;
}

/* ─────────────────────────────────────────────
   First-pass: collect all block/func declarations
   so forward calls don't produce false warnings
   ───────────────────────────────────────────── */
static void first_pass(ASTNode *node, SymTable *st, BlockRegistry *br) {
    if (!node) return;
    if (node->kind == NODE_BLOCK) {
        /* register block in registry */
        BlockInfo *bi = breg_get_or_create(br, node->block_n);
        /* scan block's direct children for first Pr/ and To/ */
        for (int i = 0; i < node->children.count; i++) {
            ASTNode *c = node->children.items[i];
            if (c->kind == NODE_PR && !bi->has_pr && c->children.count > 0) {
                ASTNode *e = c->children.items[0];
                if (e->sval) strncpy(bi->pr_expr, e->sval, 255);
                bi->has_pr = true;
            }
            if (c->kind == NODE_TO && !bi->has_to && c->children.count > 0) {
                ASTNode *inner = c->children.items[0];
                /* look for string literal inside To/ */
                if (inner->kind == NODE_LITERAL_STR && inner->sval)
                    strncpy(bi->to_answer, inner->sval, 255);
                bi->has_to = true;
            }
        }
        /* register _qN as a known function */
        char qname[32];
        snprintf(qname, sizeof(qname), "_q%d", node->block_n);
        if (!sym_find(st, qname))
            sym_add(st, qname, "void", -1, false, true, false, node->line);
    }
    if (semantic_is_class_like(node->kind)) {
        const char *cn = node->sval ? node->sval : "LuClass";
        if (!sym_find(st, cn))
            sym_add(st, cn, "type", -1, false, false, true, node->line);
        for (int i = 0; i < node->children.count; i++) {
            ASTNode *m = node->children.items[i];
            char fname[256];
            if (m->kind == NODE_METHOD_DECL) {
                snprintf(fname, sizeof(fname), "%s_%s", cn, m->sval ? m->sval : "method");
                if (!sym_find(st, fname))
                    sym_add(st, fname, m->type_name ? m->type_name : "void", -1, false, true, false, m->line);
            } else if (m->kind == NODE_CONSTRUCTOR) {
                snprintf(fname, sizeof(fname), "%s_init", cn);
                if (!sym_find(st, fname)) sym_add(st, fname, "void", -1, false, true, false, m->line);
            } else if (m->kind == NODE_DESTRUCTOR) {
                snprintf(fname, sizeof(fname), "%s_deinit", cn);
                if (!sym_find(st, fname)) sym_add(st, fname, "void", -1, false, true, false, m->line);
            } else if (m->kind == NODE_OP_OVERLOAD) {
                snprintf(fname, sizeof(fname), "%s_op_%s", cn, semantic_op_mangle(m->op));
                if (!sym_find(st, fname))
                    sym_add(st, fname, m->type_name ? m->type_name : "void", -1, false, true, false, m->line);
            }
        }
    }
    if (node->kind == NODE_FUNC_DECL || node->kind == NODE_ASYNC_FUNC) {
        if (node->sval && !sym_find(st, node->sval))
            sym_add(st, node->sval, node->type_name ? node->type_name : "void",
                    -1, false, true, false, node->line);
    }
    if (node->kind == NODE_DEF_CONST && node->sval) {
        if (!sym_find(st, node->sval))
            sym_add(st, node->sval, "int", -1, false, false, true, node->line);
    }
    if (node->kind == NODE_ENUM_DECL && node->sval) {
        if (!sym_find(st, node->sval))
            sym_add(st, node->sval, "type", -1, false, false, true, node->line);
        for (int i = 0; i < node->children.count; i++) {
            ASTNode *m = node->children.items[i];
            if (!m || !m->sval) continue;
            if (!sym_find(st, m->sval))
                sym_add(st, m->sval, "int", -1, false, false, true, m->line);
            char prefixed[256];
            snprintf(prefixed, sizeof(prefixed), "%s_%s", node->sval, m->sval);
            if (!sym_find(st, prefixed))
                sym_add(st, prefixed, "int", -1, false, false, true, m->line);
        }
    }
    for (int i = 0; i < node->children.count; i++)
        first_pass(node->children.items[i], st, br);
}

/* ─────────────────────────────────────────────
   Main recursive check
   ───────────────────────────────────────────── */
static void check_node(ASTNode *node, SymTable *st, BlockRegistry *br, int scope) {
    if (!node) return;

    switch (node->kind) {

    case NODE_VAR_DECL:
        if (node->sval) {
            /* duplicate declaration in same scope */
            if (sym_find_in_scope(st, node->sval, scope))
                lu_warn(node->line, "variable '%s' already declared in this scope (line %d)",
                        node->sval,
                        sym_find_in_scope(st, node->sval, scope)->decl_line);
            sym_add(st, node->sval, node->type_name, scope, false, false, false, node->line);
            /* type-check initialiser */
            if (node->children.count > 0) {
                ASTNode *init = node->children.items[0];
                if (init->kind != NODE_LITERAL_INT) { /* arrays: skip */
                    const char *inferred = infer_type(st, init);
                    if (!types_compatible(node->type_name, inferred) &&
                        strcmp(inferred, "auto") != 0)
                        lu_warn(node->line,
                            "type mismatch: '%s' declared as '%s' but assigned '%s'",
                            node->sval, node->type_name, inferred);
                }
            }
        }
        break;

    case NODE_PTR_DECL:
        if (node->sval) {
            if (sym_find_in_scope(st, node->sval, scope))
                lu_warn(node->line, "pointer '%s' already declared in this scope",
                        node->sval);
            sym_add(st, node->sval, node->type_name, scope, true, false, false, node->line);
        }
        break;

    case NODE_CLASS_DECL:
    case NODE_INTERFACE_DECL:
    case NODE_IMPL_DECL:
    case NODE_TEMPLATE_DECL: {
        const char *cn = node->sval ? node->sval : "LuClass";
        int class_scope = node->line;
        if (!sym_find(st, cn))
            sym_add(st, cn, "type", -1, false, false, true, node->line);
        for (int i = 0; i < node->children.count; i++) {
            ASTNode *c = node->children.items[i];
            if (c->kind == NODE_VAR_DECL && c->sval) {
                sym_add(st, c->sval, c->type_name, class_scope, false, false, false, c->line);
            } else if (semantic_is_method_like(c->kind)) {
                int method_scope = c->line;
                sym_add(st, "this", cn, method_scope, true, false, false, c->line);
                for (int j = 0; j < c->children.count; j++) {
                    ASTNode *p = c->children.items[j];
                    if (p->kind == NODE_VAR_DECL && p->sval)
                        sym_add(st, p->sval, p->type_name, method_scope, false, false, false, p->line);
                    else if (p->kind == NODE_PROGRAM)
                        check_node(p, st, br, method_scope);
                }
            }
        }
        return;
    }

    case NODE_FUNC_DECL:
    case NODE_ASYNC_FUNC:
        /* body check in a new scope */
        if (node->children.count > 0) {
            /* add params first */
            for (int i = 0; i < node->children.count; i++) {
                ASTNode *c = node->children.items[i];
                if (c->kind == NODE_VAR_DECL && c->sval)
                    sym_add(st, c->sval, c->type_name, node->line, false, false, false, c->line);
            }
            /* recurse into body (last child that is NODE_PROGRAM) */
            ASTNode *body = node->children.items[node->children.count - 1];
            if (body->kind == NODE_PROGRAM)
                check_node(body, st, br, node->line);
        }
        return; /* children already handled above */

    case NODE_BLOCK:
        /* recurse children with block scope */
        for (int i = 0; i < node->children.count; i++)
            check_node(node->children.items[i], st, br, node->block_n);
        return;

    case NODE_FUNC_CALL:
        if (node->sval) {
            const char *dot = strchr(node->sval, '.');
            if (dot && dot != node->sval && dot[1]) {
                char obj[128];
                char method[128];
                size_t obj_len = (size_t)(dot - node->sval);
                if (obj_len >= sizeof(obj)) obj_len = sizeof(obj) - 1;
                memcpy(obj, node->sval, obj_len);
                obj[obj_len] = '\0';
                snprintf(method, sizeof(method), "%s", dot + 1);
                Symbol *os = sym_find(st, obj);
                if (!os) {
                    lu_warn(node->line, "method call on undeclared object '%s'", obj);
                } else {
                    char lowered[256];
                    snprintf(lowered, sizeof(lowered), "%s_%s", os->type, method);
                    if (!sym_find(st, lowered))
                        lu_warn(node->line, "method '%s' not found for type '%s'", method, os->type);
                }
            } else if (strncmp(node->sval, "lu_", 3)  != 0 &&
                       strncmp(node->sval, "__lu_", 5) != 0 &&
                       strncmp(node->sval, "_q", 2)    != 0) {
                if (!sym_find(st, node->sval))
                    lu_warn(node->line, "call to undeclared function '%s'", node->sval);
            }
        }
        break;

    case NODE_IDENT:
        if (node->sval &&
            node->sval[0] != '_' &&           /* skip generated names */
            strncmp(node->sval, "__lu_", 5) != 0) {
            char base[128];
            const char *cut = strpbrk(node->sval, ".[");
            if (cut) {
                size_t n = (size_t)(cut - node->sval);
                if (n >= sizeof(base)) n = sizeof(base) - 1;
                memcpy(base, node->sval, n);
                base[n] = '\0';
            } else {
                snprintf(base, sizeof(base), "%s", node->sval);
            }
            if (!sym_find(st, base))
                lu_warn(node->line, "use of undeclared identifier '%s'", node->sval);
        }
        break;

    case NODE_SET: {
        if (node->sval) {
            if (strchr(node->sval, '[') || strchr(node->sval, '.') || strstr(node->sval, "->"))
                break; /* composite lvalue: base expression is checked elsewhere */
            Symbol *s = sym_find(st, node->sval);
            if (!s) {
                lu_warn(node->line, "assignment to undeclared variable '%s'", node->sval);
            } else if (s->is_const) {
                lu_warn(node->line, "assignment to constant '%s'", node->sval);
            } else if (node->children.count > 0) {
                const char *inferred = infer_type(st, node->children.items[0]);
                if (!types_compatible(s->type, inferred) &&
                    strcmp(inferred, "auto") != 0)
                    lu_warn(node->line,
                        "type mismatch in Set/: '%s' is '%s', assigned '%s'",
                        node->sval, s->type, inferred);
            }
        }
        break;
    }

    case NODE_FREE:
        if (node->children.count > 0) {
            ASTNode *arg = node->children.items[0];
            if (arg->kind == NODE_IDENT && arg->sval) {
                Symbol *s = sym_find(st, arg->sval);
                if (s && !s->is_ptr)
                    lu_warn(node->line, "Free/ on non-pointer '%s'", arg->sval);
                if (!s)
                    lu_warn(node->line, "Free/ on undeclared '%s'", arg->sval);
            }
        }
        break;

    case NODE_LITERAL_COR:
        validate_cor_key(node->sval, node->line);
        break;

    case NODE_SERVER_DECL:
        /* find cor key in sval text (simple scan) */
        if (node->sval) {
            const char *cp = strstr(node->sval, "cor/");
            if (!cp) cp = strstr(node->sval, "cor:{");
            if (!cp)
                lu_warn(node->line,
                    "Server #q%d declared without a cor/ key (verification disabled)",
                    node->block_n);
        }
        break;

    case NODE_LINREF:
        /* verify referenced block exists in registry */
        if (node->sval && br) {
            /* sval is the expression text passed to Def:anw(), e.g. "2+2" */
            /* We can only warn if no block with a matching Pr/ exists */
            bool found = false;
            for (int i = 0; i < br->count; i++) {
                if (br->entries[i].has_pr &&
                    strstr(br->entries[i].pr_expr, node->sval))
                    found = true;
            }
            if (!found && br->count > 0)
                lu_warn(node->line,
                    "Lin reference to '%s' — no block with matching Pr/ found",
                    node->sval);
        }
        break;

    case NODE_RETURN:
        /* could check return type against enclosing function — future work */
        break;

    default:
        break;
    }

    /* recurse children (unless already handled above) */
    for (int i = 0; i < node->children.count; i++)
        check_node(node->children.items[i], st, br, scope);
}

/* ─────────────────────────────────────────────
   Public entry point
   ───────────────────────────────────────────── */
bool semantic_check(ASTNode *root, SymTable *st) {
    /* ── Pre-populate C / Lu built-ins ── */
    sym_add(st, "malloc",   "ptr",  -1, true,  true,  false, 0);
    sym_add(st, "calloc",   "ptr",  -1, true,  true,  false, 0);
    sym_add(st, "realloc",  "ptr",  -1, true,  true,  false, 0);
    sym_add(st, "free",     "void", -1, false, true,  false, 0);
    sym_add(st, "memset",   "void", -1, false, true,  false, 0);
    sym_add(st, "memcpy",   "void", -1, false, true,  false, 0);
    sym_add(st, "printf",   "int",  -1, false, true,  false, 0);
    sym_add(st, "fprintf",  "int",  -1, false, true,  false, 0);
    sym_add(st, "snprintf", "int",  -1, false, true,  false, 0);
    sym_add(st, "strlen",   "int",  -1, false, true,  false, 0);
    sym_add(st, "strcmp",   "int",  -1, false, true,  false, 0);
    sym_add(st, "strcpy",   "str",  -1, false, true,  false, 0);
    sym_add(st, "strncpy",  "str",  -1, false, true,  false, 0);
    sym_add(st, "strcat",   "str",  -1, false, true,  false, 0);
    sym_add(st, "strdup",   "str",  -1, false, true,  false, 0);
    sym_add(st, "atoi",     "int",  -1, false, true,  false, 0);
    sym_add(st, "atoll",    "int64",-1, false, true,  false, 0);
    sym_add(st, "atof",     "float",-1, false, true,  false, 0);
    sym_add(st, "exit",     "void", -1, false, true,  false, 0);
    /* C runtime globals / self-hosting runtime helpers */
    sym_add(st, "stderr",   "ptr",  -1, true,  false, false, 0);
    sym_add(st, "stdin",    "ptr",  -1, true,  false, false, 0);
    sym_add(st, "stdout",   "ptr",  -1, true,  false, false, 0);
    sym_add(st, "__luc_argc", "int", -1, false, false, false, 0);
    sym_add(st, "__luc_argv", "str", -1, false, false, false, 0);
    sym_add(st, "LUC_FPRINTF", "int", -1, false, true, false, 0);
    sym_add(st, "luc_fopen",   "ptr", -1, true,  true, false, 0);
    sym_add(st, "luc_fread",   "int", -1, false, true, false, 0);
    sym_add(st, "luc_fclose",  "void",-1, false, true, false, 0);
    sym_add(st, "lu_self_lex_cast",     "int",  -1, false, true, false, 0);
    sym_add(st, "lu_self_codegen_cast", "void", -1, false, true, false, 0);
    /* Lu runtime */
    sym_add(st, "lu_send",       "void", -1, false, true, false, 0);
    sym_add(st, "lu_recv",       "void", -1, false, true, false, 0);
    sym_add(st, "lu_broadcast",  "void", -1, false, true, false, 0);
    sym_add(st, "lu_route",      "void", -1, false, true, false, 0);
    sym_add(st, "lu_inq",        "void", -1, false, true, false, 0);
    sym_add(st, "lu_log",        "void", -1, false, true, false, 0);
    sym_add(st, "lu_throw",      "void", -1, false, true, false, 0);
    sym_add(st, "lu_assert",     "void", -1, false, true, false, 0);
    sym_add(st, "lu_user_new",   "auto", -1, false, true, false, 0);
    sym_add(st, "lu_server_new", "ptr",  -1, true,  true, false, 0);
    sym_add(st, "lu_spawn",      "void", -1, false, true, false, 0);
    sym_add(st, "lu_chan_new",   "ptr",  -1, true,  true, false, 0);
    sym_add(st, "lu_chan_send",      "void", -1, false, true, false, 0);
    sym_add(st, "lu_chan_send_safe", "void", -1, false, true, false, 0);
    sym_add(st, "lu_chan_recv",  "ptr",  -1, true,  true, false, 0);
    sym_add(st, "lu_event_new",  "ptr",  -1, true,  true, false, 0);
    sym_add(st, "lu_event_emit", "void", -1, false, true, false, 0);
    sym_add(st, "lu_event_on",   "void", -1, false, true, false, 0);
    sym_add(st, "lu_parse_ip",   "ip",   -1, false, true, false, 0);
    sym_add(st, "lu_make_cor",   "cor",  -1, false, true, false, 0);
    sym_add(st, "lu_print_int",  "void", -1, false, true, false, 0);
    sym_add(st, "lu_print_float","void", -1, false, true, false, 0);
    sym_add(st, "lu_print_str",  "void", -1, false, true, false, 0);
    sym_add(st, "lu_print_bool", "void", -1, false, true, false, 0);
    sym_add(st, "__lu_trace",      "void", -1, false, true, false, 0);
    sym_add(st, "__lu_breakpoint", "void", -1, false, true, false, 0);
    sym_add(st, "__lu_watch",      "void", -1, false, true, false, 0);
    /* Lu error codes */
    sym_add(st, "ERR_COR",  "int", -1, false, false, true, 0);
    sym_add(st, "ERR_IP",   "int", -1, false, false, true, 0);
    sym_add(st, "ERR_MEM",  "int", -1, false, false, true, 0);
    sym_add(st, "ERR_MSG",  "int", -1, false, false, true, 0);
    sym_add(st, "ERR_AUTH", "int", -1, false, false, true, 0);

    BlockRegistry br = {0};

    /* ── Pass 1: collect all declarations forward ── */
    first_pass(root, st, &br);

    /* ── Pass 2: semantic checks ── */
    check_node(root, st, &br, -1);

    return true; /* non-fatal (warnings only) */
}
