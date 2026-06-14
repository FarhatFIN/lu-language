#include "lu.h"
#include <stdarg.h>

/* ─────────────────────────────────────────────
   Helpers
   ───────────────────────────────────────────── */
static void emit(Codegen *cg, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
}
static void indent(Codegen *cg) {
    for (int i = 0; i < cg->indent; i++) emit(cg, "    ");
}
static void iemit(Codegen *cg, const char *fmt, ...) {
    indent(cg);
    va_list ap; va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
}

/* Lu type → C type string */
static const char *lu2c(const char *t) {
    if (!t) return "int";
    if (!strcmp(t,"int"))   return "int";
    if (!strcmp(t,"int64")) return "int64_t";
    if (!strcmp(t,"float")) return "double";
    if (!strcmp(t,"str"))   return "char*";
    if (!strcmp(t,"bool"))  return "bool";
    if (!strcmp(t,"byte"))  return "uint8_t";
    if (!strcmp(t,"ip"))    return "lu_ip_t";
    if (!strcmp(t,"id"))    return "char*";
    if (!strcmp(t,"cor"))   return "lu_cor_t";
    if (!strcmp(t,"msg"))   return "lu_msg_t";
    if (!strcmp(t,"lib"))   return "void*";
    if (!strcmp(t,"void"))  return "void";
    return t;
}

static bool node_is_class_like(ASTNode *n) {
    return n && (n->kind == NODE_CLASS_DECL || n->kind == NODE_INTERFACE_DECL ||
                 n->kind == NODE_IMPL_DECL || n->kind == NODE_TEMPLATE_DECL);
}

static const char *class_field_c_type(ASTNode *cls, const char *t) {
    if (cls && cls->annot && t && !strcmp(cls->annot, t))
        return "void*"; /* minimal C backend for template<T> fields */
    return lu2c(t);
}

static Symbol *cg_sym_find(Codegen *cg, const char *name);

static void emit_c_ident(Codegen *cg, const char *name) {
    if (!name || !*name) { emit(cg, "_unknown"); return; }
    if (!strncmp(name, "this.", 5)) {
        emit(cg, "this->%s", name + 5);
        return;
    }
    if (!strncmp(name, "super.", 6)) {
        emit(cg, "this->base.%s", name + 6);
        return;
    }

    /* If a pointer variable is used with dot syntax (p.x), lower it to p->x.
       This keeps Lu ergonomic while still generating correct C. */
    const char *dot = strchr(name, '.');
    if (dot && dot != name && dot[1]) {
        char obj[128];
        size_t obj_len = (size_t)(dot - name);
        if (obj_len >= sizeof(obj)) obj_len = sizeof(obj) - 1;
        memcpy(obj, name, obj_len);
        obj[obj_len] = '\0';
        Symbol *sym = cg_sym_find(cg, obj);
        if (sym && sym->is_ptr) {
            emit(cg, "%s->%s", obj, dot + 1);
            return;
        }
    }

    emit(cg, "%s", name);
}

static void cg_sym_add(Codegen *cg, const char *name, const char *type, bool is_ptr) {
    if (!cg || !name || !*name || cg->syms.count >= SYM_MAX) return;
    for (int i = cg->syms.count - 1; i >= 0; i--) {
        if (!strcmp(cg->syms.entries[i].name, name)) {
            strncpy(cg->syms.entries[i].type, type ? type : "auto", sizeof(cg->syms.entries[i].type) - 1);
            cg->syms.entries[i].is_ptr = is_ptr;
            return;
        }
    }
    Symbol *s = &cg->syms.entries[cg->syms.count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    strncpy(s->type, type ? type : "auto", sizeof(s->type) - 1);
    s->is_ptr = is_ptr;
}

static Symbol *cg_sym_find(Codegen *cg, const char *name) {
    if (!cg || !name) return NULL;
    for (int i = cg->syms.count - 1; i >= 0; i--)
        if (!strcmp(cg->syms.entries[i].name, name)) return &cg->syms.entries[i];
    return NULL;
}

static const char *op_mangle(const char *op) {
    if (!op) return "op";
    if (!strcmp(op, "+"))  return "add";
    if (!strcmp(op, "-"))  return "sub";
    if (!strcmp(op, "*"))  return "mul";
    if (!strcmp(op, "/"))  return "div";
    if (!strcmp(op, "%"))  return "mod";
    if (!strcmp(op, "==")) return "eq";
    if (!strcmp(op, "!=")) return "neq";
    if (!strcmp(op, "<"))  return "lt";
    if (!strcmp(op, ">"))  return "gt";
    if (!strcmp(op, "<=")) return "le";
    if (!strcmp(op, ">=")) return "ge";
    return "op";
}

static void emit_c_string_literal(Codegen *cg, const char *v) {
    emit(cg, "\"");
    if (!v) v = "";
    for (const unsigned char *cp = (const unsigned char *)v; *cp; cp++) {
        switch (*cp) {
        case '\n': emit(cg, "\\n"); break;
        case '\t': emit(cg, "\\t"); break;
        case '\r': emit(cg, "\\r"); break;
        case '\\': emit(cg, "\\\\"); break;
        case '"':  emit(cg, "\\\""); break;
        default:
            if (*cp < 32) emit(cg, "\\x%02x", *cp);
            else fputc(*cp, cg->out);
            break;
        }
    }
    emit(cg, "\"");
}

/* forward */
static void gen_node(Codegen *cg, ASTNode *n);
static void gen_expr(Codegen *cg, ASTNode *n);

/* ─────────────────────────────────────────────
   Expression emitter
   ───────────────────────────────────────────── */
static void gen_expr(Codegen *cg, ASTNode *n) {
    if (!n) { emit(cg, "0"); return; }
    switch (n->kind) {
    case NODE_LITERAL_INT:   emit(cg, "%lld", (long long)n->ival); break;
    case NODE_LITERAL_FLOAT: emit(cg, "%g", n->fval); break;
    case NODE_LITERAL_BOOL:  emit(cg, "%s", n->bval ? "true" : "false"); break;
    case NODE_LITERAL_STR: {
        const char *v = n->sval ? n->sval : "";
        if (v[0] == '$') { emit(cg, "/* template */ "); emit_c_string_literal(cg, v + 1); break; }
        emit_c_string_literal(cg, v);
        break;
    }
    case NODE_LITERAL_IP:
        emit(cg, "lu_parse_ip(\"%s\")", n->sval ? n->sval : "");
        break;
    case NODE_LITERAL_COR: {
        /* count commas to determine array length */
        int cor_len = 1;
        if (n->sval) for (const char *cp = n->sval; *cp; cp++) if (*cp == ',') cor_len++;
        emit(cg, "lu_make_cor((int[]){%s}, %d)",
             n->sval ? n->sval : "0", cor_len);
        break;
    }
    case NODE_IDENT:
        emit_c_ident(cg, n->sval ? n->sval : "_unknown");
        break;
    case NODE_EXPR_BINOP:
        if (n->op && !strcmp(n->op, "**")) {
            emit(cg, "lu_pow_num((double)(");
            gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
            emit(cg, "), (int64_t)(");
            gen_expr(cg, n->children.count > 1 ? n->children.items[1] : NULL);
            emit(cg, "))");
        } else {
            emit(cg, "(");
            gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
            emit(cg, " %s ", n->op ? n->op : "+");
            gen_expr(cg, n->children.count > 1 ? n->children.items[1] : NULL);
            emit(cg, ")");
        }
        break;
    case NODE_EXPR_UNOP: {
        const char *op = n->op ? n->op : "!";
        if (strstr(op, "_post")) {
            gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
            emit(cg, "%s", strncmp(op, "++", 2) == 0 ? "++" : "--");
        } else {
            emit(cg, "%s", op);
            gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        }
        break;
    }
    case NODE_EXPR_TERNARY:
        emit(cg, "(");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, " ? ");
        gen_expr(cg, n->children.count > 1 ? n->children.items[1] : NULL);
        emit(cg, " : ");
        gen_expr(cg, n->children.count > 2 ? n->children.items[2] : NULL);
        emit(cg, ")");
        break;
    case NODE_EXPR_FIELD:
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, "%s%s", n->op ? n->op : ".", n->sval ? n->sval : "field");
        break;
    case NODE_EXPR_INDEX:
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, "[");
        gen_expr(cg, n->children.count > 1 ? n->children.items[1] : NULL);
        emit(cg, "]");
        break;
    case NODE_EXPR_REF:
        emit(cg, "(&");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ")");
        break;
    case NODE_EXPR_DEREF:
        emit(cg, "(*");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ")");
        break;
    case NODE_FUNC_CALL: {
        const char *name = n->sval ? n->sval : "fn";
        const char *dot = strchr(name, '.');
        if (dot && dot != name && dot[1]) {
            char obj[128];
            char method[128];
            size_t obj_len = (size_t)(dot - name);
            if (obj_len >= sizeof(obj)) obj_len = sizeof(obj) - 1;
            memcpy(obj, name, obj_len);
            obj[obj_len] = '\0';
            snprintf(method, sizeof(method), "%s", dot + 1);
            Symbol *sym = cg_sym_find(cg, obj);
            const char *ty = sym ? sym->type : obj;
            emit(cg, "%s_%s(", ty, method);
            if (sym && sym->is_ptr) emit(cg, "%s", obj);
            else emit(cg, "&%s", obj);
            for (int i = 0; i < n->children.count; i++) {
                emit(cg, ", ");
                gen_expr(cg, n->children.items[i]);
            }
            emit(cg, ")");
        } else {
            emit(cg, "%s(", name);
            for (int i = 0; i < n->children.count; i++) {
                if (i) emit(cg, ", ");
                gen_expr(cg, n->children.items[i]);
            }
            emit(cg, ")");
        }
        break;
    }
    case NODE_ALLOC:
        /* Alloc/type:count — usable as an expression (returns pointer) */
        emit(cg, "malloc(sizeof(%s) * (", lu2c(n->type_name));
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, "))");
        break;
    case NODE_NEW_EXPR:
        /* new Type(args...) — minimal C++-style heap allocation. Constructor calls are explicit: Type_init(ptr, ...). */
        emit(cg, "calloc(1, sizeof(%s))", n->sval ? n->sval : "char");
        break;
    default:
        gen_node(cg, n); break;
    }
}

/* ─────────────────────────────────────────────
   Statement / declaration emitter
   ───────────────────────────────────────────── */
static void gen_block_body(Codegen *cg, ASTNode *n) {
    for (int i = 0; i < n->children.count; i++)
        gen_node(cg, n->children.items[i]);
}

static int emit_param_list(Codegen *cg, ASTNode *fn, const char *self_type) {
    bool first = true;
    int body_start = fn ? fn->children.count : 0;

    if (self_type && *self_type) {
        emit(cg, "%s *this", self_type);
        first = false;
    }

    for (int i = 0; fn && i < fn->children.count; i++) {
        ASTNode *c = fn->children.items[i];
        if (c->kind == NODE_VAR_DECL) {
            if (!first) emit(cg, ", ");
            emit(cg, "%s %s", lu2c(c->type_name), c->sval ? c->sval : "_p");
            first = false;
            body_start = i + 1;
        } else {
            body_start = i;
            break;
        }
    }

    if (first) emit(cg, "void");
    return body_start;
}

static void emit_function_signature(Codegen *cg, ASTNode *fn, const char *name, const char *self_type) {
    emit(cg, "%s %s(", lu2c(fn ? fn->type_name : "void"), name ? name : "_fn");
    emit_param_list(cg, fn, self_type);
    emit(cg, ")");
}

static void gen_function_definition_named(Codegen *cg, ASTNode *fn, const char *name, const char *self_type) {
    int body_start;
    emit(cg, "\n%s %s(", lu2c(fn ? fn->type_name : "void"), name ? name : "_fn");
    body_start = emit_param_list(cg, fn, self_type);
    emit(cg, ") {\n");
    cg->indent++;
    for (int i = body_start; fn && i < fn->children.count; i++)
        gen_node(cg, fn->children.items[i]);
    cg->indent--;
    emit(cg, "}\n");
}

static void gen_class_struct_decl(Codegen *cg, ASTNode *cls) {
    const char *name = cls && cls->sval ? cls->sval : "LuClass";

    if (cls && cls->kind == NODE_INTERFACE_DECL) {
        emit(cg, "\n/* interface %s: C backend emits function-table compatible struct shell */\n", name);
    }
    if (cls && cls->kind == NODE_TEMPLATE_DECL && cls->annot) {
        emit(cg, "\n/* template<%s> lowered to void* fields in the C backend */\n", cls->annot);
    }

    emit(cg, "\ntypedef struct %s %s;\n", name, name);
    emit(cg, "struct %s {\n", name);
    cg->indent++;

    for (int i = 0; cls && i < cls->children.count; i++) {
        ASTNode *c = cls->children.items[i];
        if (c->kind == NODE_INHERIT && c->sval && c->sval[0]) {
            char base[128];
            snprintf(base, sizeof(base), "%s", c->sval);
            char *comma = strchr(base, ',');
            if (comma) *comma = '\0';
            iemit(cg, "%s base;\n", base);
        }
    }

    for (int i = 0; cls && i < cls->children.count; i++) {
        ASTNode *f = cls->children.items[i];
        if (f->kind == NODE_VAR_DECL)
            iemit(cg, "%s %s;\n", class_field_c_type(cls, f->type_name), f->sval ? f->sval : "_field");
    }

    cg->indent--;
    emit(cg, "};\n");
}

static void gen_class_method_prototypes(Codegen *cg, ASTNode *cls) {
    const char *cn = cls && cls->sval ? cls->sval : "LuClass";
    for (int i = 0; cls && i < cls->children.count; i++) {
        ASTNode *m = cls->children.items[i];
        char name[256];
        if (m->kind == NODE_METHOD_DECL) {
            snprintf(name, sizeof(name), "%s_%s", cn, m->sval ? m->sval : "method");
            emit_function_signature(cg, m, name, cn);
            emit(cg, ";\n");
        } else if (m->kind == NODE_CONSTRUCTOR) {
            snprintf(name, sizeof(name), "%s_init", cn);
            emit(cg, "void %s(", name);
            emit_param_list(cg, m, cn);
            emit(cg, ");\n");
        } else if (m->kind == NODE_DESTRUCTOR) {
            snprintf(name, sizeof(name), "%s_deinit", cn);
            emit(cg, "void %s(%s *this);\n", name, cn);
        } else if (m->kind == NODE_OP_OVERLOAD) {
            snprintf(name, sizeof(name), "%s_op_%s", cn, op_mangle(m->op));
            emit_function_signature(cg, m, name, cn);
            emit(cg, ";\n");
        }
    }
}

static void gen_class_method_definitions(Codegen *cg, ASTNode *cls) {
    const char *cn = cls && cls->sval ? cls->sval : "LuClass";
    for (int i = 0; cls && i < cls->children.count; i++) {
        ASTNode *m = cls->children.items[i];
        char name[256];
        if (m->kind == NODE_METHOD_DECL) {
            snprintf(name, sizeof(name), "%s_%s", cn, m->sval ? m->sval : "method");
            gen_function_definition_named(cg, m, name, cn);
        } else if (m->kind == NODE_CONSTRUCTOR) {
            snprintf(name, sizeof(name), "%s_init", cn);
            m->type_name = m->type_name ? m->type_name : lu_strdup("void");
            gen_function_definition_named(cg, m, name, cn);
        } else if (m->kind == NODE_DESTRUCTOR) {
            snprintf(name, sizeof(name), "%s_deinit", cn);
            m->type_name = m->type_name ? m->type_name : lu_strdup("void");
            gen_function_definition_named(cg, m, name, cn);
        } else if (m->kind == NODE_OP_OVERLOAD) {
            snprintf(name, sizeof(name), "%s_op_%s", cn, op_mangle(m->op));
            gen_function_definition_named(cg, m, name, cn);
        }
    }
}

static bool top_level_skip_after_preamble(ASTNode *n) {
    if (!n) return true;
    switch (n->kind) {
    case NODE_LANG_DECL: case NODE_IMPORT: case NODE_MODE: case NODE_OPT:
    case NODE_ANNOT: case NODE_MODULE: case NODE_DEFINE:
    case NODE_DEF_CONST: case NODE_DEF_CONFIG:
    case NODE_STRUCT_DECL: case NODE_ENUM_DECL:
    case NODE_FUNC_DECL: case NODE_ASYNC_FUNC:
        return true;
    default:
        return node_is_class_like(n);
    }
}

static void gen_top_level(Codegen *cg, ASTNode *root) {
    /* Constants/config first. */
    for (int i = 0; root && i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (n->kind == NODE_DEFINE || n->kind == NODE_DEF_CONST ||
            n->kind == NODE_DEF_CONFIG || n->kind == NODE_MODE)
            gen_node(cg, n);
    }

    /* Types before prototypes and blocks. */
    for (int i = 0; root && i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (n->kind == NODE_STRUCT_DECL || n->kind == NODE_ENUM_DECL)
            gen_node(cg, n);
        else if (node_is_class_like(n))
            gen_class_struct_decl(cg, n);
    }

    /* Function and method prototypes: fixes calls to functions declared later. */
    emit(cg, "\n/* Lu forward declarations */\n");
    for (int i = 0; root && i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (n->kind == NODE_FUNC_DECL || n->kind == NODE_ASYNC_FUNC) {
            emit_function_signature(cg, n, n->sval ? n->sval : "_fn", NULL);
            emit(cg, ";\n");
        } else if (node_is_class_like(n)) {
            gen_class_method_prototypes(cg, n);
        } else if (n->kind == NODE_BLOCK) {
            emit(cg, "void _q%d(void);\n", n->block_n);
        }
    }
    emit(cg, "\n");

    /* Function and method definitions before executable blocks. */
    for (int i = 0; root && i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (n->kind == NODE_FUNC_DECL || n->kind == NODE_ASYNC_FUNC)
            gen_function_definition_named(cg, n, n->sval ? n->sval : "_fn", NULL);
        else if (node_is_class_like(n))
            gen_class_method_definitions(cg, n);
    }

    /* Blocks and other executable top-level nodes keep source order. */
    for (int i = 0; root && i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (!top_level_skip_after_preamble(n))
            gen_node(cg, n);
    }
}

static void gen_node(Codegen *cg, ASTNode *n) {
    if (!n) return;

    switch (n->kind) {

    /* ── Top-level ignored for codegen ── */
    case NODE_LANG_DECL:
    case NODE_IMPORT:
        /* already emitted in pre-pass above forward-decls — skip here */
        return;
    case NODE_MODULE:
    case NODE_OPT:      /* handled at init */
    case NODE_ANNOT:    /* handled at init */
        return;

    case NODE_PROGRAM:
        for (int i = 0; i < n->children.count; i++)
            gen_node(cg, n->children.items[i]);
        return;

    /* ── Preprocessor ── */
    case NODE_DEFINE:
        emit(cg, "#define %s ", n->sval ? n->sval : "X");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, "\n");
        return;
    case NODE_INCLUDE:
        emit(cg, "#include %s\n", n->sval ? n->sval : "");
        return;

    /* ── Const / config ── */
    case NODE_DEF_CONST:
        iemit(cg, "static const int %s = ", n->sval ? n->sval : "_const");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ";\n");
        return;
    case NODE_DEF_CONFIG:
        iemit(cg, "/* config: %s = ", n->sval ? n->sval : "");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, " */\n");
        return;

    /* ── Mode ── */
    case NODE_MODE:
        iemit(cg, "/* mode: %s */\n", n->sval ? n->sval : "");
        if (n->sval && !strcmp(n->sval, "*debug*"))
            cg->debug_mode = true;
        return;

    /* ── Block #q[n] → void _qN(void) ── */
    case NODE_BLOCK: {
        emit(cg, "\nvoid _q%d(void) {\n", n->block_n);
        cg->indent++;
        if (cg->debug_mode)
            iemit(cg, "fprintf(stderr, \"[LU] entering block #q%d\\n\");\n", n->block_n);
        for (int i = 0; i < n->children.count; i++)
            gen_node(cg, n->children.items[i]);
        cg->indent--;
        emit(cg, "}\n");
        /* register block (bounded) */
        if (cg->block_count < (int)(sizeof(cg->blocks) / sizeof(cg->blocks[0])))
            cg->blocks[cg->block_count++] = n->block_n;
        else
            lu_warn(n->line, "block registry full, #q%d not auto-called from main()", n->block_n);
        return;
    }

    /* ── Variable declaration ── */
    case NODE_VAR_DECL: {
        cg_sym_add(cg, n->sval ? n->sval : "_var", n->type_name, false);
        iemit(cg, "%s %s", lu2c(n->type_name), n->sval ? n->sval : "_var");
        if (n->children.count > 0) {
            ASTNode *first = n->children.items[0];
            bool is_array = (n->op && !strcmp(n->op, "array"));
            if (is_array) {
                /* Lu: int arr[3] = {1,2,3}; int arr[n];
                   C cannot initialise a VLA, so arr[n] = {...} becomes arr[] = {...}. */
                bool has_init = n->children.count > 1;
                bool literal_size = first && first->kind == NODE_LITERAL_INT;
                if (has_init && !literal_size) {
                    emit(cg, "[]");
                } else {
                    emit(cg, "[");
                    gen_expr(cg, first);
                    emit(cg, "]");
                }
                if (has_init) {
                    emit(cg, " = {");
                    for (int i = 1; i < n->children.count; i++) {
                        if (i > 1) emit(cg, ", ");
                        gen_expr(cg, n->children.items[i]);
                    }
                    emit(cg, "}");
                }
            } else {
                emit(cg, " = ");
                gen_expr(cg, first);
            }
        }
        emit(cg, ";\n");
        return;
    }

    /* ── Pointer declaration ── */
    case NODE_PTR_DECL: {
        cg_sym_add(cg, n->sval ? n->sval : "_ptr", n->type_name, true);
        iemit(cg, "%s *%s", lu2c(n->type_name), n->sval ? n->sval : "_ptr");
        if (n->children.count > 0) { emit(cg, " = "); gen_expr(cg, n->children.items[0]); }
        emit(cg, ";\n");
        return;
    }

    /* ── Function declaration ── */
    case NODE_FUNC_DECL:
    case NODE_ASYNC_FUNC:
        gen_function_definition_named(cg, n, n->sval ? n->sval : "_fn", NULL);
        return;

    /* ── If ── */
    case NODE_IF: {
        iemit(cg, "if (");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ") {\n");
        cg->indent++;
        if (n->children.count > 1) gen_block_body(cg, n->children.items[1]);
        cg->indent--;
        iemit(cg, "}");
        if (n->children.count > 2) {
            ASTNode *alt = n->children.items[2];
            if (alt->kind == NODE_IF) {
                emit(cg, " else ");
                cg->indent++; gen_node(cg, alt); cg->indent--;
                return;
            } else {
                emit(cg, " else {\n");
                cg->indent++;
                gen_block_body(cg, alt);
                cg->indent--;
                iemit(cg, "}\n");
                return;
            }
        }
        emit(cg, "\n");
        return;
    }

    /* ── Loops ── */
    case NODE_LOOP: {
        int li = cg->label_counter++;
        iemit(cg, "for (int _i%d = 0; _i%d < ", li, li);
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, "; _i%d++) {\n", li);
        cg->indent++;
        if (n->children.count > 1) gen_block_body(cg, n->children.items[1]);
        cg->indent--;
        iemit(cg, "}\n");
        return;
    }
    case NODE_LOOP_WHILE: {
        iemit(cg, "while (");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ") {\n");
        cg->indent++;
        if (n->children.count > 1) gen_block_body(cg, n->children.items[1]);
        cg->indent--;
        iemit(cg, "}\n");
        return;
    }
    case NODE_LOOP_EACH: {
        /* for (typeof(*list) item : list) — use C99 pointer iteration */
        ASTNode *item = n->children.count > 0 ? n->children.items[0] : NULL;
        ASTNode *list = n->children.count > 1 ? n->children.items[1] : NULL;
        int li = cg->label_counter++;
        iemit(cg, "/* Loop/Each */\n");
        iemit(cg, "for (int _ei%d = 0; _ei%d < (int)(sizeof(", li, li);
        gen_expr(cg, list);
        emit(cg, ")/sizeof(*(");
        gen_expr(cg, list);
        emit(cg, "))); _ei%d++) {\n", li);
        cg->indent++;
        iemit(cg, "__typeof__(*(");
        gen_expr(cg, list);
        emit(cg, ")) %s = (", item ? item->sval : "_item");
        gen_expr(cg, list);
        emit(cg, ")[_ei%d];\n", li);
        if (n->children.count > 2) gen_block_body(cg, n->children.items[2]);
        cg->indent--;
        iemit(cg, "}\n");
        return;
    }

    /* ── GAME ENGINE команды ── */

    /* cr(obj;zombie)exp → создать структуру объекта */
    case NODE_CR: {
        /* cr(obj;name) → struct LuObj_name name = {0}; */
        char *arg = n->sval ? n->sval : "unknown";
        /* parse "obj;name" → extract name after ; */
        char *sep = strchr(arg, ';');
        const char *obj_name = sep ? sep + 1 : arg;
        iemit(cg, "/* cr: создать объект %s */\n", obj_name);
        iemit(cg, "LuObj_%s %s = {0};\n", obj_name, obj_name);
        iemit(cg, "%s.active = true;\n", obj_name);
        iemit(cg, "%s.hp = %s_DEFAULT_HP;\n", obj_name, obj_name);
        return;
    }

    /* dam(obj;name)(vl"-15") → нанести урон */
    case NODE_DAM: {
        char *arg = n->sval ? n->sval : "unknown";
        char *sep = strchr(arg, ';');
        const char *obj_name = sep ? sep + 1 : arg;
        iemit(cg, "/* dam: урон объекту %s */\n", obj_name);
        if (n->children.count > 0) {
            iemit(cg, "%s.hp += ", obj_name); /* value already negative */
            gen_expr(cg, n->children.items[0]);
            emit(cg, ";\n");
            iemit(cg, "if (%s.hp <= 0) { %s.active = false; %s.hp = 0; }\n",
                  obj_name, obj_name, obj_name);
        }
        return;
    }

    /* onl(obj;name) → guard: только если объект активен */
    case NODE_ONL: {
        char *arg = n->sval ? n->sval : "unknown";
        char *sep = strchr(arg, ';');
        const char *obj_name = sep ? sep + 1 : arg;
        iemit(cg, "/* onl: только если %s активен */\n", obj_name);
        iemit(cg, "if (%s.active) {\n", obj_name);
        cg->indent++;
        for (int i = 0; i < n->children.count; i++)
            gen_node(cg, n->children.items[i]);
        cg->indent--;
        iemit(cg, "}\n");
        return;
    }

    /* func(run) → вызов метода */
    case NODE_FUNC_CALL_GAME: {
        iemit(cg, "/* func: вызов %s */\n", n->sval ? n->sval : "");
        iemit(cg, "lu_func_%s(", n->sval ? n->sval : "unknown");
        for (int i = 0; i < n->children.count; i++) {
            if (i > 0) emit(cg, ", ");
            gen_expr(cg, n->children.items[i]);
        }
        emit(cg, ");\n");
        return;
    }

    /* exp → активировать/заспавнить объект */
    case NODE_EXP: {
        iemit(cg, "/* exp: активация объекта */\n");
        return;
    }

    /* cl → вызов блока */
    case NODE_CL: {
        iemit(cg, "_q%s();\n", n->sval ? n->sval : "1");
        return;
    }

    /* Pr/ → printf ── */
    case NODE_PR: {
        if (n->children.count == 0) { iemit(cg, "printf(\"\\n\");\n"); return; }
        ASTNode *e = n->children.items[0];
        if (e->kind == NODE_LITERAL_STR) {
            const char *v = e->sval ? e->sval : "";
            if (v[0] == '$') v++;
            iemit(cg, "printf(\"%%s\\n\", ");
            emit_c_string_literal(cg, v);
            emit(cg, ");\n");
        } else if (e->kind == NODE_LITERAL_INT) {
            iemit(cg, "printf(\"%%lld\\n\", (long long)%lld);\n", (long long)e->ival);
        } else if (e->kind == NODE_LITERAL_FLOAT) {
            iemit(cg, "printf(\"%%g\\n\", (double)%g);\n", e->fval);
        } else if (e->kind == NODE_LITERAL_BOOL) {
            iemit(cg, "printf(\"%%s\\n\", %s ? \"true\" : \"false\");\n",
                  e->bval ? "true" : "false");
        } else {
            iemit(cg, "lu_print(");
            gen_expr(cg, e);
            emit(cg, ");\n");
        }
        return;
    }

    /* ── To/ → inline child statement ── */
    case NODE_TO:
        if (n->children.count > 0) gen_node(cg, n->children.items[0]);
        return;

    /* ── Set/ → assignment ── */
    case NODE_SET: {
        indent(cg);
        emit_c_ident(cg, n->sval ? n->sval : "_var");
        emit(cg, " = ");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ";\n");
        return;
    }

    /* ── Return ── */
    case NODE_RETURN:
        if (n->sval && !strcmp(n->sval, "__break__")) {
            iemit(cg, "break;\n");
            return;
        }
        if (n->children.count == 0) {
            iemit(cg, "return;\n");
            return;
        }
        iemit(cg, "return ");
        gen_expr(cg, n->children.items[0]);
        emit(cg, ";\n");
        return;

    /* ── Function call statement ── */
    case NODE_FUNC_CALL:
        iemit(cg, "");
        gen_expr(cg, n);
        emit(cg, ";\n");
        return;

    /* ── Lin reference: Def:anw(expr)/Lin ── */
    case NODE_LINREF: {
        /* sval = the expression from Def:anw(...) e.g. "2+2" */
        /* Look up in block registry for a block whose Pr/ matches */
        const char *expr = n->sval ? n->sval : "";
        int found_q = -1;
        for (int i = 0; i < cg->breg.count; i++) {
            BlockInfo *bi = &cg->breg.entries[i];
            if (bi->has_pr && strstr(bi->pr_expr, expr)) {
                found_q = bi->block_n;
                break;
            }
        }
        if (found_q >= 0) {
            iemit(cg, "/* Lin: borrow logic from #q%d for '%s' */\n", found_q, expr);
            iemit(cg, "_q%d();\n", found_q);
        } else {
            iemit(cg, "/* Lin ref '%s': no matching block found */\n", expr);
        }
        return;
    }

    /* ── User declaration ── */
    case NODE_USER_DECL:
        iemit(cg, "/* User: %s */\n", n->sval ? n->sval : "");
        iemit(cg, "lu_user_t _user = lu_user_new(\"%s\");\n",
              n->sval ? n->sval : "");
        return;

    /* ── Server declaration ── */
    case NODE_SERVER_DECL:
        iemit(cg, "lu_server_t *_server_q%d = lu_server_new(%d, \"%s\");\n",
              n->block_n, n->block_n, n->sval ? n->sval : "");
        return;

    /* ── Network send ── */
    case NODE_SEND: {
        if (n->children.count > 0 && n->children.items[0]->kind == NODE_LITERAL_IP) {
            iemit(cg, "lu_send(\"%s\", &(", n->sval ? n->sval : "");
            gen_expr(cg, n->children.items[0]);
            emit(cg, "));\n");
        } else {
            iemit(cg, "lu_send(\"%s\", NULL);\n", n->sval ? n->sval : "");
        }
        return;
    }

    /* ── Network recv ── */
    case NODE_RECV:
        iemit(cg, "lu_recv(");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ");\n");
        return;

    /* ── Bcast ── */
    case NODE_BCAST:
        iemit(cg, "lu_broadcast(\"%s\");\n", n->sval ? n->sval : "");
        return;

    /* ── Route ── */
    case NODE_ROUTE:
        iemit(cg, "lu_route(");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ", ");
        gen_expr(cg, n->children.count > 1 ? n->children.items[1] : NULL);
        emit(cg, ");\n");
        return;

    /* ── inq; ── */
    case NODE_INQ:
        iemit(cg, "lu_inq(\"%s\");\n", n->sval ? n->sval : "");
        return;

    /* ── Memory management ── */
    case NODE_ALLOC: {
        /* used as expression in var decl; here emit as standalone call */
        iemit(cg, "malloc(sizeof(%s) * (", lu2c(n->type_name));
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, "));\n");
        return;
    }
    case NODE_FREE:
        iemit(cg, "free(");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ");\n");
        return;
    case NODE_MEMSET:
        iemit(cg, "memset(");
        for (int i = 0; i < n->children.count; i++) {
            if (i) emit(cg, ", ");
            gen_expr(cg, n->children.items[i]);
        }
        emit(cg, ");\n");
        return;
    case NODE_MEMCPY:
        iemit(cg, "memcpy(");
        for (int i = 0; i < n->children.count; i++) {
            if (i) emit(cg, ", ");
            gen_expr(cg, n->children.items[i]);
        }
        emit(cg, ");\n");
        return;

    /* ── Try/Catch/Finally ── */
    case NODE_TRY: {
        int label = cg->label_counter++;
        iemit(cg, "{\n");
        cg->indent++;
        iemit(cg, "lu_exc_ctx_t _lu_try%d;\n", label);
        iemit(cg, "_lu_try%d.prev = __lu_exc_stack;\n", label);
        iemit(cg, "__lu_exc_stack = &_lu_try%d;\n", label);
        iemit(cg, "int _lu_state%d = setjmp(_lu_try%d.env);\n", label, label);
        iemit(cg, "int _lu_handled%d = 0;\n", label);
        iemit(cg, "if (_lu_state%d == 0) {\n", label);
        cg->indent++;
        if (n->children.count > 0) gen_block_body(cg, n->children.items[0]);
        cg->indent--;
        iemit(cg, "}\n");
        iemit(cg, "__lu_exc_stack = _lu_try%d.prev;\n", label);

        for (int i = 1; i < n->children.count; i++) {
            ASTNode *c = n->children.items[i];
            if (!c->sval) continue; /* finally handled after catches */
            iemit(cg, "if (_lu_state%d != 0 && !_lu_handled%d", label, label);
            if (c->sval[0] && strncmp(c->sval, "err", 3) && strcmp(c->sval, "error") && strcmp(c->sval, "any")) {
                if (!strncmp(c->sval, "ERR_", 4)) emit(cg, " && __lu_exc_code == %s", c->sval);
            }
            emit(cg, ") {\n");
            cg->indent++;
            iemit(cg, "_lu_handled%d = 1;\n", label);
            gen_block_body(cg, c);
            cg->indent--;
            iemit(cg, "}\n");
        }

        for (int i = 1; i < n->children.count; i++) {
            ASTNode *c = n->children.items[i];
            if (c->sval) continue;
            iemit(cg, "{\n");
            cg->indent++;
            gen_block_body(cg, c);
            cg->indent--;
            iemit(cg, "}\n");
        }

        iemit(cg, "if (_lu_state%d != 0 && !_lu_handled%d) lu_throw(__lu_exc_code, __lu_exc_msg);\n", label, label);
        cg->indent--;
        iemit(cg, "}\n");
        return;
    }

    /* ── Throw ── */
    case NODE_THROW:
        iemit(cg, "lu_throw(");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        if (n->children.count > 1) {
            emit(cg, ", ");
            gen_expr(cg, n->children.items[1]);
        } else emit(cg, ", \"error\"");
        emit(cg, ");\n");
        return;

    /* ── Struct ── */
    case NODE_STRUCT_DECL: {
        emit(cg, "\ntypedef struct {\n");
        cg->indent++;
        for (int i = 0; i < n->children.count; i++) {
            ASTNode *f = n->children.items[i];
            iemit(cg, "%s %s;\n", lu2c(f->type_name), f->sval ? f->sval : "_f");
        }
        cg->indent--;
        emit(cg, "} %s;\n", n->sval ? n->sval : "_struct");
        return;
    }

    /* ── Enum ── */
    case NODE_ENUM_DECL: {
        emit(cg, "\ntypedef enum {\n");
        cg->indent++;
        for (int i = 0; i < n->children.count; i++) {
            ASTNode *m = n->children.items[i];
            const char *ename = n->sval ? n->sval : "E";
            const char *mname = m->sval ? m->sval : "V";
            iemit(cg, "%s_%s", ename, mname);
            if (m->bval) emit(cg, " = %lld", (long long)m->ival);
            emit(cg, ",\n");
            /* C++-like convenience: allow both Color_RED and RED in Lu code. */
            iemit(cg, "%s = %s_%s", mname, ename, mname);
            if (i < n->children.count - 1) emit(cg, ",");
            emit(cg, "\n");
        }
        cg->indent--;
        emit(cg, "} %s;\n", n->sval ? n->sval : "_enum");
        return;
    }

    /* ── Namespace ── */
    case NODE_NAMESPACE: {
        emit(cg, "/* namespace %s */\n", n->sval ? n->sval : "");
        for (int i = 0; i < n->children.count; i++) gen_node(cg, n->children.items[i]);
        return;
    }

    /* ── Async ── */
    case NODE_AWAIT:
        iemit(cg, "/* await */ ");
        if (n->children.count > 0) gen_node(cg, n->children.items[0]);
        return;
    case NODE_SPAWN:
        iemit(cg, "lu_spawn(");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ");\n");
        return;

    /* ── Channels ── */
    case NODE_CHAN_DECL:
        iemit(cg, "lu_chan_t *%s = lu_chan_new();\n", n->sval ? n->sval : "_ch");
        return;
    case NODE_CHAN_SEND:
        iemit(cg, "lu_chan_send_safe(%s, (void*)(intptr_t)(", n->sval ? n->sval : "_ch");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, "));\n");
        return;
    case NODE_CHAN_RECV:
        iemit(cg, "lu_chan_recv(%s);\n", n->sval ? n->sval : "_ch");
        return;

    /* ── Events ── */
    case NODE_EVENT:
        iemit(cg, "lu_event_t *ev_%s = lu_event_new(\"%s\");\n",
              n->sval ? n->sval : "ev", n->sval ? n->sval : "ev");
        return;
    case NODE_EMIT:
        iemit(cg, "lu_event_emit(ev_%s, ", n->sval ? n->sval : "ev");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ");\n");
        return;
    case NODE_ON:
        iemit(cg, "lu_event_on(ev_%s, ", n->sval ? n->sval : "ev");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ");\n");
        return;
    case NODE_SELECT:
        iemit(cg, "/* select */ {\n");
        cg->indent++;
        for (int i = 0; i < n->children.count; i++) gen_node(cg, n->children.items[i]);
        cg->indent--;
        iemit(cg, "}\n");
        return;

    /* ── Log ── */
    case NODE_LOG: {
        const char *lvl = n->sval ? n->sval : "info";
        if (n->children.count == 0) {
            iemit(cg, "lu_log(\"%s\", \"\");\n", lvl);
            return;
        }
        ASTNode *msg = n->children.items[0];
        if (msg->kind == NODE_LITERAL_STR) {
            /* string literal: pass directly */
            iemit(cg, "lu_log(\"%s\", \"%s\");\n", lvl,
                  msg->sval ? msg->sval : "");
        } else {
            /* non-string expression: convert via snprintf into temp buffer */
            int tmp = cg->tmp_counter++;
            iemit(cg, "{ char _lulog%d[256]; snprintf(_lulog%d, 256, \"%%s: \", \"%s\");\n",
                  tmp, tmp, lvl);
            cg->indent++;
            iemit(cg, "/* expr */ lu_log(\"%s\", _lulog%d); }\n", lvl, tmp);
            cg->indent--;
            /* also emit the expression as a separate lu_print so value is visible */
            iemit(cg, "lu_print(");
            gen_expr(cg, msg);
            emit(cg, ");\n");
        }
        return;
    }

    /* ── Assert ── */
    case NODE_ASSERT:
        iemit(cg, "lu_assert(");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ", ");
        if (n->children.count > 1) gen_expr(cg, n->children.items[1]);
        else emit(cg, "\"assertion failed\"");
        emit(cg, ", %d);\n", n->line);
        return;

    /* ── C++-inspired class lowering ── */
    case NODE_CLASS_DECL:
    case NODE_INTERFACE_DECL:
    case NODE_IMPL_DECL:
    case NODE_TEMPLATE_DECL:
        gen_class_struct_decl(cg, n);
        gen_class_method_prototypes(cg, n);
        gen_class_method_definitions(cg, n);
        return;

    case NODE_NEW_EXPR:
        iemit(cg, "");
        gen_expr(cg, n);
        emit(cg, ";\n");
        return;

    case NODE_DELETE_EXPR:
        iemit(cg, "free(%s);\n", n->sval ? n->sval : "NULL");
        return;

    default:
        iemit(cg, "/* unhandled node kind %d */\n", n->kind);
        return;
    }
}

/* ─────────────────────────────────────────────
   RUNTIME HEADER (preamble)
   ───────────────────────────────────────────── */
static void emit_runtime(Codegen *cg) {
    /* Use fputs (not emit/vfprintf) so that % in the runtime source is preserved literally */
    fputs(
"/* ============================================================\n"
"   Lu Runtime — generated by luc (Lu Compiler)\n"
"   ============================================================ */\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"#include <ctype.h>\n"
"#include <stdint.h>\n"
"#include <stdbool.h>\n"
"#include <stdarg.h>\n"
"#include <setjmp.h>\n"
"/* Lu global argc/argv */\n"
"int   __luc_argc = 0;\n"
"char **__luc_argv = NULL;\n"
"\n"
"/* ── Lu types ── */\n"
"typedef struct { uint8_t a, b, c, d; uint16_t port; } lu_ip_t;\n"
"typedef struct { int *vals; int len; }                  lu_cor_t;\n"
"typedef struct { char *data; int len; }                 lu_msg_t;\n"
"typedef struct { char nic[64]; char name[64]; lu_ip_t ip; } lu_user_t;\n"
"typedef struct { int id; char meta[256]; }              lu_server_t;\n"
"typedef struct { void **buf; int head, tail, cap; }     lu_chan_t;\n"
"typedef struct { char name[64]; void(*cb)(void*); }    lu_event_t;\n"
"\n"
"/* ── Runtime error codes ── */\n"
"#define ERR_COR  1\n"
"#define ERR_IP   2\n"
"#define ERR_MEM  3\n"
"#define ERR_MSG  4\n"
"#define ERR_AUTH 5\n"
"\n"
"/* ── Runtime functions ── */\n"
"typedef struct lu_exc_ctx { jmp_buf env; struct lu_exc_ctx *prev; } lu_exc_ctx_t;\n"
"static lu_exc_ctx_t *__lu_exc_stack = NULL;\n"
"static int __lu_exc_code = 0;\n"
"static const char *__lu_exc_msg = \"\";\n"
"\n"
"static double lu_pow_num(double base, int64_t exp) {\n"
"    double result = 1.0;\n"
"    bool neg = exp < 0;\n"
"    uint64_t e = neg ? (uint64_t)(-exp) : (uint64_t)exp;\n"
"    while (e) {\n"
"        if (e & 1u) result *= base;\n"
"        base *= base;\n"
"        e >>= 1u;\n"
"    }\n"
"    return neg ? 1.0 / result : result;\n"
"}\n"
"\n"
"static lu_ip_t lu_parse_ip(const char *s) {\n"
"    lu_ip_t ip = {0};\n"
"    if (sscanf(s, \"%hhu.%hhu.%hhu.%hhu:%hu\", &ip.a, &ip.b, &ip.c, &ip.d, &ip.port) < 4)\n"
"        sscanf(s, \"%hhu.%hhu.%hhu.%hu\", &ip.a, &ip.b, &ip.c, &ip.port);\n"
"    return ip;\n"
"}\n"
"\n"
"static lu_cor_t lu_make_cor(int *vals, int len) {\n"
"    lu_cor_t c; c.vals = vals; c.len = len; return c;\n"
"}\n"
"\n"
"static lu_user_t lu_user_new(const char *attrs) {\n"
"    lu_user_t u = {0}; strncpy(u.nic, attrs, 63); return u;\n"
"}\n"
"\n"
"static lu_server_t *lu_server_new(int id, const char *meta) {\n"
"    lu_server_t *s = calloc(1, sizeof(lu_server_t));\n"
"    if (!s) { fprintf(stderr, \"[Lu MEM] out of memory\\n\"); exit(ERR_MEM); }\n"
"    s->id = id; strncpy(s->meta, meta ? meta : \"\", sizeof(s->meta) - 1);\n"
"    return s;\n"
"}\n"
"\n"
"static void lu_send(const char *data, lu_ip_t *dest) {\n"
"    if (dest)\n"
"        printf(\"[Lu NET] send '%s' -> %d.%d.%d.%d:%d\\n\",\n"
"               data, dest->a, dest->b, dest->c, dest->d, dest->port);\n"
"    else\n"
"        printf(\"[Lu NET] send '%s'\\n\", data);\n"
"}\n"
"\n"
"static void lu_recv(const char *expected) {\n"
"    printf(\"[Lu NET] recv '%s'\\n\", expected ? expected : \"\");\n"
"}\n"
"\n"
"static void lu_broadcast(const char *data) {\n"
"    printf(\"[Lu NET] broadcast '%s'\\n\", data);\n"
"}\n"
"\n"
"static void lu_route(lu_ip_t dst, lu_ip_t via) {\n"
"    printf(\"[Lu NET] route %d.%d.%d.%d:%d via %d.%d.%d.%d:%d\\n\",\n"
"           dst.a,dst.b,dst.c,dst.d,dst.port,via.a,via.b,via.c,via.d,via.port);\n"
"}\n"
"\n"
"static void lu_inq(const char *req) {\n"
"    printf(\"[Lu NET] inq: %s\\n\", req);\n"
"}\n"
"\n"
"static void lu_log(const char *level, const char *msg) {\n"
"    const char *col = \"\\033[0m\";\n"
"    if (!strcmp(level,\"err\")  || !strcmp(level,\"fatal\")) col=\"\\033[31m\";\n"
"    else if (!strcmp(level,\"warn\")) col=\"\\033[33m\";\n"
"    else if (!strcmp(level,\"debug\")) col=\"\\033[36m\";\n"
"    printf(\"%s[Lu %s]\\033[0m %s\\n\", col, level, msg);\n"
"    if (!strcmp(level,\"fatal\")) exit(1);\n"
"}\n"
"\n"
"static void lu_throw(int code, const char *msg) {\n"
"    __lu_exc_code = code;\n"
"    __lu_exc_msg = msg ? msg : \"error\";\n"
"    if (__lu_exc_stack) longjmp(__lu_exc_stack->env, 1);\n"
"    fprintf(stderr, \"[Lu THROW] code=%d: %s\\n\", code, __lu_exc_msg);\n"
"    exit(code ? code : 1);\n"
"}\n"
"\n"
"static void lu_assert(bool cond, const char *msg, int line) {\n"
"    if (!cond) {\n"
"        fprintf(stderr, \"[Lu ASSERT] line %d: %s\\n\", line, msg);\n"
"        exit(1);\n"
"    }\n"
"}\n"
"\n"
"static void lu_print_int(int64_t v)    { printf(\"%lld\\n\", (long long)v); }\n"
"static void lu_print_float(double v)   { printf(\"%g\\n\", v); }\n"
"static void lu_print_str(const char*v) { printf(\"%s\\n\", v ? v : \"\"); }\n"
"static void lu_print_bool(bool v)      { printf(\"%s\\n\", v?\"true\":\"false\"); }\n"
"#define lu_print(x) _Generic((x), \\\n"
"    int:lu_print_int, int64_t:lu_print_int,\\\n"
"    long long:lu_print_int, double:lu_print_float,\\\n"
"    float:lu_print_float, bool:lu_print_bool,\\\n"
"    char*:lu_print_str, default:lu_print_int)(x)\n"
"\n"
"static void lu_spawn(void (*fn)(void)) { if (fn) fn(); }\n"
"\n"
"static lu_chan_t *lu_chan_new(void) {\n"
"    lu_chan_t *c = calloc(1, sizeof(lu_chan_t));\n"
"    if (!c) { fprintf(stderr, \"[Lu MEM] out of memory\\n\"); exit(ERR_MEM); }\n"
"    c->cap = 16; c->buf = calloc(16, sizeof(void*));\n"
"    if (!c->buf) { fprintf(stderr, \"[Lu MEM] out of memory\\n\"); exit(ERR_MEM); }\n"
"    return c;\n"
"}\n"
"static void lu_chan_send(lu_chan_t *c, void *v) {\n"
"    if (!c) return;\n"
"    c->buf[c->tail % c->cap] = v;\n"
"    c->tail++;\n"
"}\n"
"static void *lu_chan_recv(lu_chan_t *c) {\n"
"    if (!c || c->head == c->tail) return NULL;\n"
"    return c->buf[c->head++ % c->cap];\n"
"}\n"
"\n"
"static lu_event_t *lu_event_new(const char *name) {\n"
"    lu_event_t *e = calloc(1, sizeof(lu_event_t));\n"
"    if (!e) { fprintf(stderr, \"[Lu MEM] out of memory\\n\"); exit(ERR_MEM); }\n"
"    strncpy(e->name, name ? name : \"\", 63); return e;\n"
"}\n"
"static void lu_event_emit(lu_event_t *e, void *data) {\n"
"    if (e && e->cb) e->cb(data);\n"
"}\n"
"static void lu_event_on(lu_event_t *e, void(*cb)(void*)) {\n"
"    if (e) e->cb = cb;\n"
"}\n"
"\n"
"static void __lu_trace(const char *what) {\n"
"    fprintf(stderr, \"[Lu TRACE] %s\\n\", what);\n"
"}\n"
"static void __lu_breakpoint(int line) {\n"
"    fprintf(stderr, \"[Lu BREAK] line %d\\n\", line);\n"
"}\n"
"static void __lu_watch(const char *var) {\n"
"    fprintf(stderr, \"[Lu WATCH] %s changed\\n\", var);\n"
"}\n"
"\n"
"/* ── cor/ verification ── */\n"
"static bool lu_cor_verify(lu_cor_t a, lu_cor_t b) {\n"
"    if (a.len != b.len) return false;\n"
"    for (int i = 0; i < a.len; i++)\n"
"        if (a.vals[i] != b.vals[i]) return false;\n"
"    return true;\n"
"}\n"
"\n"
"/* ── IP helpers ── */\n"
"static bool lu_ip_eq(lu_ip_t a, lu_ip_t b) {\n"
"    return a.a==b.a && a.b==b.b && a.c==b.c && a.d==b.d && a.port==b.port;\n"
"}\n"
"static void lu_ip_print(lu_ip_t ip) {\n"
"    printf(\"%d.%d.%d.%d:%d\", ip.a, ip.b, ip.c, ip.d, ip.port);\n"
"}\n"
"\n"
"/* ── User helpers ── */\n"
"static void lu_user_set_name(lu_user_t *u, const char *name) {\n"
"    if (u && name) strncpy(u->name, name, 63);\n"
"}\n"
"static void lu_user_set_ip(lu_user_t *u, lu_ip_t ip) {\n"
"    if (u) u->ip = ip;\n"
"}\n"
"static void lu_user_print(lu_user_t u) {\n"
"    printf(\"User{nic='%s' name='%s' ip=%d.%d.%d.%d:%d}\\n\",\n"
"           u.nic, u.name, u.ip.a, u.ip.b, u.ip.c, u.ip.d, u.ip.port);\n"
"}\n"
"\n"
"/* ── Chan grow ── */\n"
"static void lu_chan_grow(lu_chan_t *c) {\n"
"    int newcap = c->cap * 2;\n"
"    void **grown = realloc(c->buf, sizeof(void*) * newcap);\n"
"    if (!grown) { fprintf(stderr, \"[Lu MEM] out of memory\\n\"); exit(ERR_MEM); }\n"
"    c->buf = grown;\n"
"    c->cap = newcap;\n"
"}\n"
"/* safe chan_send with auto-grow */\n"
"static void lu_chan_send_safe(lu_chan_t *c, void *v) {\n"
"    if (!c) return;\n"
"    if (c->tail - c->head >= c->cap) lu_chan_grow(c);\n"
"    c->buf[c->tail % c->cap] = v;\n"
"    c->tail++;\n"
"}\n"
"\n"
, cg->out);
}

/* ─────────────────────────────────────────────
   Collect Opt/ and @annotations before codegen
   ───────────────────────────────────────────── */
static void collect_meta(Codegen *cg, ASTNode *root) {
    for (int i = 0; i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (n->kind == NODE_OPT && n->sval) {
            if (!strcmp(n->sval, "O0")) cg->opt_level = 0;
            else if (!strcmp(n->sval, "O1")) cg->opt_level = 1;
            else if (!strcmp(n->sval, "O2")) cg->opt_level = 2;
            else if (!strcmp(n->sval, "O3")) cg->opt_level = 3;
        }
        if (n->kind == NODE_MODE && n->sval && !strcmp(n->sval, "*debug*"))
            cg->debug_mode = true;
    }
}

/* ─────────────────────────────────────────────
   Pre-pass: build BlockRegistry from all #q blocks
   ───────────────────────────────────────────── */
static void collect_blocks(Codegen *cg, ASTNode *node) {
    if (!node) return;
    if (node->kind == NODE_BLOCK) {
        BlockInfo *bi = breg_get_or_create(&cg->breg, node->block_n);
        if (!bi) return;
        for (int i = 0; i < node->children.count; i++) {
            ASTNode *c = node->children.items[i];
            if (c->kind == NODE_PR && !bi->has_pr && c->children.count > 0) {
                ASTNode *e = c->children.items[0];
                const char *v = e->sval ? e->sval : "";
                strncpy(bi->pr_expr, v, 255);
                bi->has_pr = true;
            }
            if (c->kind == NODE_TO && !bi->has_to && c->children.count > 0) {
                ASTNode *inner = c->children.items[0];
                if (inner->kind == NODE_LITERAL_STR && inner->sval) {
                    strncpy(bi->to_answer, inner->sval, 255);
                    bi->has_to = true;
                }
            }
        }
    }
    for (int i = 0; i < node->children.count; i++)
        collect_blocks(cg, node->children.items[i]);
}

/* ─────────────────────────────────────────────
   ENTRY POINT
   ───────────────────────────────────────────── */
void codegen_run(ASTNode *root, FILE *out, int opt_level, bool debug) {
    Codegen cg = {0};
    cg.out        = out;
    cg.opt_level  = opt_level;
    cg.debug_mode = debug;
    cg.cur_block  = -1;

    collect_meta(&cg, root);

    /* ── Pre-pass: build block registry (needed for Lin references) ── */
    collect_blocks(&cg, root);

    /* emit only real C header imports before the runtime.
       Bare imports such as `Import Russian` are Lu language/profile imports,
       not C headers, so they must not become `#include "Russian"`. */
    for (int i = 0; i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (n->kind == NODE_IMPORT && n->sval) {
            const char *v = n->sval;
            if (v[0] == '"' || v[0] == '<')
                emit(&cg, "#include %s\n", v);
        }
    }
    emit(&cg, "\n");

    /* emit runtime */
    emit_runtime(&cg);

    /* emit optimization hint comment */
    if (cg.opt_level >= 0)
        emit(&cg, "/* Lu opt level: O%d */\n\n", cg.opt_level);

    /* generate all top-level declarations in C-safe order:
       constants/types -> prototypes -> functions/methods -> blocks */
    gen_top_level(&cg, root);

    /* emit main() that calls blocks in order */
    emit(&cg,
"\nint main(int argc, char **argv) {\n"
"    __luc_argc = argc; __luc_argv = argv;\n"
"    /* Lu entry point: call all #q blocks in order */\n"
);
    /* call all blocks in registration order */
    if (cg.block_count > 0) {
        for (int i = 0; i < cg.block_count; i++) {
            emit(&cg, "    _q%d();\n", cg.blocks[i]);
        }
    } else {
        emit(&cg, "    /* no blocks defined */\n");
    }
    emit(&cg, "    return 0;\n}\n");
}