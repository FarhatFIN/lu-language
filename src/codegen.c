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

/* Like lu2c, but for a variable declaration: insert the variable name
   in the right place for function pointer types.
   For fn:int,int->int with name "op" → "int (*op)(int,int)".
   For other types, returns lu2c(t) followed by " name". */
static void emit_c_decl(Codegen *cg, const char *t, const char *name);

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
    if (!strcmp(t,"auto"))  return "int"; /* fallback, usually overridden */
    /* Optional ?T → lowered to T* (NULL = no value, non-NULL = has value).
       This is a simplification of Zig's ?T but works for the common case. */
    if (t[0] == '?') {
        static char obuf[128];
        const char *inner = t + 1;
        const char *c_inner = lu2c(inner);
        snprintf(obuf, sizeof(obuf), "%s*", c_inner);
        return obuf;
    }
    /* Error union !T → lowered to T (errors handled via exceptions).
       This is a simplification — real Zig !T carries an error code. */
    if (t[0] == '!') {
        return lu2c(t + 1);
    }
    /* Function pointer type: "fn:params->ret" e.g. "fn:int,int->int"
       For a bare type (e.g. function param), return "ret (*)(params)".
       For a variable declaration "fn:... name", the caller must use
       lu2c_fn_decl instead so the name goes inside the parens. */
    if (!strncmp(t, "fn:", 3)) {
        static char fbuf[256];
        const char *params_start = t + 3;
        const char *arrow = strstr(params_start, "->");
        if (!arrow) return "void*";
        const char *ret = arrow + 2;
        size_t plen = (size_t)(arrow - params_start);
        if (plen > 200) plen = 200;
        snprintf(fbuf, sizeof(fbuf), "%s (*", ret);
        size_t rlen = strlen(fbuf);
        memcpy(fbuf + rlen, ")(", 2); rlen += 2;
        if (plen >= 2 && params_start[0] == '(' && params_start[plen-1] == ')') {
            size_t inner = plen - 2;
            if (inner > sizeof(fbuf) - rlen - 2) inner = sizeof(fbuf) - rlen - 2;
            memcpy(fbuf + rlen, params_start + 1, inner);
            rlen += inner;
        }
        fbuf[rlen] = ')';
        fbuf[rlen+1] = '\0';
        return fbuf;
    }
    /* Vector<T> → lu_vector_<T> */
    if (!strncmp(t, "Vector<", 7)) {
        static char buf[128];
        const char *inner = t + 7;
        const char *end = strchr(inner, '>');
        if (end) {
            size_t ilen = (size_t)(end - inner);
            if (ilen > 120) ilen = 120;
            memcpy(buf, "lu_vector_", 10);
            memcpy(buf + 10, inner, ilen);
            buf[10 + ilen] = '\0';
            return buf;
        }
    }
    /* Unique<T> → Unique(T) macro (auto-freeing pointer) */
    if (!strncmp(t, "Unique<", 7)) {
        static char ubuf[128];
        const char *inner = t + 7;
        const char *end = strchr(inner, '>');
        if (end) {
            size_t ilen = (size_t)(end - inner);
            if (ilen > 120) ilen = 120;
            memcpy(ubuf, "Unique(", 7);
            memcpy(ubuf + 7, inner, ilen);
            ubuf[7 + ilen] = ')';
            ubuf[8 + ilen] = '\0';
            return ubuf;
        }
    }
    /* Shared<T> → Shared(T) macro (refcounted pointer) */
    if (!strncmp(t, "Shared<", 7)) {
        static char sbuf[128];
        const char *inner = t + 7;
        const char *end = strchr(inner, '>');
        if (end) {
            size_t ilen = (size_t)(end - inner);
            if (ilen > 120) ilen = 120;
            memcpy(sbuf, "Shared(", 7);
            memcpy(sbuf + 7, inner, ilen);
            sbuf[7 + ilen] = ')';
            sbuf[8 + ilen] = '\0';
            return sbuf;
        }
    }
    return t;
}

/* Emit a C declaration: type + name. For function pointer types,
   the name goes inside the parens: int (*name)(int,int).
   For everything else, it's just "type name". */
static void emit_c_decl(Codegen *cg, const char *t, const char *name) {
    if (t && !strncmp(t, "fn:", 3)) {
        /* Function pointer: build "ret (*name)(params)" */
        const char *params_start = t + 3;
        const char *arrow = strstr(params_start, "->");
        if (!arrow) { emit(cg, "void* %s", name ? name : "_v"); return; }
        const char *ret = arrow + 2;
        size_t plen = (size_t)(arrow - params_start);
        emit(cg, "%s (*%s)(", ret, name ? name : "_v");
        /* Copy params without surrounding () */
        if (plen >= 2 && params_start[0] == '(' && params_start[plen-1] == ')') {
            for (size_t i = 1; i < plen - 1; i++)
                fputc(params_start[i], cg->out);
        }
        emit(cg, ")");
    } else {
        emit(cg, "%s %s", lu2c(t), name ? name : "_v");
    }
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

/* Like class_field_c_type, but also substitutes T with void* in method
   signatures of template classes.  Used by emit_param_list and the
   method signature emitters. */
static const char *class_method_c_type(ASTNode *cls, const char *t) {
    return class_field_c_type(cls, t);
}

static Symbol *cg_sym_find(Codegen *cg, const char *name);

/* Forward declarations for class registry (used by emit_c_ident). */
static ASTNode *cg_class_find(Codegen *cg, const char *name);
static const char *cg_class_base_name(ASTNode *cls);

/* emit_c_ident: translate a Lu lvalue/identifier string into a valid C expression.
   Handles this./super. anywhere in the string (not just at the start), and lowers
   ptr.field to ptr->field when the base is a known pointer variable. */
static void emit_c_ident(Codegen *cg, const char *name) {
    if (!name || !*name) { emit(cg, "_unknown"); return; }

    /* Fast path: simple identifier with no dot/bracket — most common case. */
    if (!strpbrk(name, ".[")) {
        emit(cg, "%s", name);
        return;
    }

    /* Walk the string and translate every this. → this-> and super. → this->base. */
    char buf[1024];
    size_t bi = 0;
    size_t i = 0;
    size_t nlen = strlen(name);
    while (i < nlen && bi < sizeof(buf) - 8) {
        /* this. → this-> */
        if (!strncmp(name + i, "this.", 5)) {
            memcpy(buf + bi, "this->", 6); bi += 6; i += 5;
            continue;
        }
        /* super. → this->base. */
        if (!strncmp(name + i, "super.", 6)) {
            memcpy(buf + bi, "this->base.", 11); bi += 11; i += 6;
            continue;
        }
        buf[bi++] = name[i++];
    }
    buf[bi] = '\0';

    /* Now check if the FIRST dotted component is a known pointer variable.
       If so, lower the first . to -> (only the first; the rest of the chain
       is on the pointed-to struct, which uses .). */
    /* For `this->field`, the dot was already replaced by -> above, so we
       need to look for either `.` or `->` as the separator. */
    const char *dot = strchr(buf, '.');
    const char *arrow = strstr(buf, "->");
    /* Use whichever comes first. */
    const char *sep = NULL;
    if (dot && arrow) sep = (dot < arrow) ? dot : arrow;
    else if (dot) sep = dot;
    else if (arrow) sep = arrow;
    if (sep && sep != buf && sep[1]) {
        /* Extract the object name (everything before the separator).
           For "->", the object is everything before "->". */
        char obj[128];
        size_t obj_len;
        int sep_advance; /* how many chars to skip after the separator */
        if (sep == arrow) {
            obj_len = (size_t)(arrow - buf);
            sep_advance = 2; /* skip "->" */
        } else {
            obj_len = (size_t)(dot - buf);
            sep_advance = 1; /* skip "." */
        }
        if (obj_len >= sizeof(obj)) obj_len = sizeof(obj) - 1;
        memcpy(obj, buf, obj_len);
        obj[obj_len] = '\0';
        const char *field = sep + sep_advance;

        /* For "this->field" (already lowered from "this.field"), check if
           the field is inherited — if so, rewrite to "this->base.field". */
        if (!strcmp(obj, "this") && cg->current_class) {
            const char *end = strpbrk(field, ".[");
            size_t flen = end ? (size_t)(end - field) : strlen(field);
            char fname[128];
            if (flen >= sizeof(fname)) flen = sizeof(fname) - 1;
            memcpy(fname, field, flen);
            fname[flen] = '\0';
            bool found_here = false;
            ASTNode *cls = cg->current_class;
            for (int i = 0; cls && i < cls->children.count; i++) {
                ASTNode *f = cls->children.items[i];
                if (f->kind == NODE_VAR_DECL && f->sval &&
                    !strcmp(f->sval, fname)) {
                    found_here = true;
                    break;
                }
            }
            if (!found_here && cls) {
                ASTNode *cur = cls;
                ASTNode *visited[64]; /* cycle detection */
                int nvisited = 0;
                char prefix[256] = "";
                bool found = false;
                while (cur && nvisited < 64) {
                    /* Check for cycle */
                    bool cycle = false;
                    for (int v = 0; v < nvisited; v++)
                        if (visited[v] == cur) { cycle = true; break; }
                    if (cycle) break;
                    visited[nvisited++] = cur;
                    const char *base_name = cg_class_base_name(cur);
                    if (!base_name) break;
                    strncat(prefix, "base.", sizeof(prefix) - strlen(prefix) - 1);
                    cur = cg_class_find(cg, base_name);
                    if (!cur) break;
                    for (int i = 0; i < cur->children.count; i++) {
                        ASTNode *f = cur->children.items[i];
                        if (f->kind == NODE_VAR_DECL && f->sval &&
                            !strcmp(f->sval, fname)) {
                            found = true;
                            break;
                        }
                    }
                    if (found) {
                        emit(cg, "this->%s%s", prefix, field);
                        return;
                    }
                }
            }
        }

        /* skip "this" and "super" — they are already lowered to this-> ... */
        if (strcmp(obj, "this") != 0 && strcmp(obj, "this->base") != 0) {
            Symbol *sym = cg_sym_find(cg, obj);
            if (sym && sym->is_ptr) {
                emit(cg, "%s->%s", obj, field);
                return;
            }
            /* Inherited field: if obj is a class instance and the field
               is not declared in the class itself but in a base class,
               rewrite to obj.base.field. Walk the class hierarchy. */
            if (sym) {
                ASTNode *cls = cg_class_find(cg, sym->type);
                if (cls) {
                    const char *end = strpbrk(field, ".[");
                    size_t flen = end ? (size_t)(end - field) : strlen(field);
                    char fname[128];
                    if (flen >= sizeof(fname)) flen = sizeof(fname) - 1;
                    memcpy(fname, field, flen);
                    fname[flen] = '\0';
                    bool found_here = false;
                    for (int i = 0; i < cls->children.count; i++) {
                        ASTNode *f = cls->children.items[i];
                        if (f->kind == NODE_VAR_DECL && f->sval &&
                            !strcmp(f->sval, fname)) {
                            found_here = true;
                            break;
                        }
                    }
                    if (!found_here) {
                        ASTNode *cur = cls;
                        ASTNode *visited2[64]; /* cycle detection */
                        int nvisited2 = 0;
                        char prefix[256] = "";
                        bool found = false;
                        while (cur && nvisited2 < 64) {
                            bool cycle = false;
                            for (int v = 0; v < nvisited2; v++)
                                if (visited2[v] == cur) { cycle = true; break; }
                            if (cycle) break;
                            visited2[nvisited2++] = cur;
                            const char *base_name = cg_class_base_name(cur);
                            if (!base_name) break;
                            strncat(prefix, "base.", sizeof(prefix) - strlen(prefix) - 1);
                            cur = cg_class_find(cg, base_name);
                            if (!cur) break;
                            for (int i = 0; i < cur->children.count; i++) {
                                ASTNode *f = cur->children.items[i];
                                if (f->kind == NODE_VAR_DECL && f->sval &&
                                    !strcmp(f->sval, fname)) {
                                    found = true;
                                    break;
                                }
                            }
                            if (found) {
                                emit(cg, "%s.%s%s", obj, prefix, field);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    emit(cg, "%s", buf);
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

/* Class registry: register a class AST node by name. */
static void cg_class_register(Codegen *cg, const char *name, ASTNode *node) {
    if (!cg || !name || cg->class_count >= CLASS_REG_MAX) return;
    /* Don't register duplicates. */
    for (int i = 0; i < cg->class_count; i++)
        if (!strcmp(cg->classes[i].name, name)) return;
    strncpy(cg->classes[cg->class_count].name, name, 127);
    cg->classes[cg->class_count].name[127] = '\0';
    cg->classes[cg->class_count].node = node;
    cg->class_count++;
}

/* Find a class AST node by name. Returns NULL if not found. */
static ASTNode *cg_class_find(Codegen *cg, const char *name) {
    if (!cg || !name) return NULL;
    for (int i = 0; i < cg->class_count; i++)
        if (!strcmp(cg->classes[i].name, name)) return cg->classes[i].node;
    return NULL;
}

/* Find the base class name of a class (the first "extends X" target).
   Returns NULL if the class has no base. */
static const char *cg_class_base_name(ASTNode *cls) {
    if (!cls) return NULL;
    for (int i = 0; i < cls->children.count; i++) {
        ASTNode *c = cls->children.items[i];
        if (c->kind == NODE_INHERIT && c->sval && c->sval[0]) {
            /* bases is comma-separated; return the first one */
            static char first[128];
            const char *comma = strchr(c->sval, ',');
            size_t len = comma ? (size_t)(comma - c->sval) : strlen(c->sval);
            if (len >= sizeof(first)) len = sizeof(first) - 1;
            memcpy(first, c->sval, len);
            first[len] = '\0';
            return first;
        }
    }
    return NULL;
}

/* Check if a class declares a method with the given name. */
static bool cg_class_has_method(ASTNode *cls, const char *method) {
    if (!cls || !method) return false;
    for (int i = 0; i < cls->children.count; i++) {
        ASTNode *m = cls->children.items[i];
        if (m->kind == NODE_METHOD_DECL && m->sval && !strcmp(m->sval, method))
            return true;
        if (m->kind == NODE_CONSTRUCTOR && !strcmp(method, "init"))
            return true;
        if (m->kind == NODE_DESTRUCTOR && !strcmp(method, "deinit"))
            return true;
    }
    return false;
}

/* Resolve a method call: walk the class hierarchy starting from `cls`,
   and return the class that actually declares the method.
   Returns NULL if not found anywhere in the chain. */
static ASTNode *cg_class_resolve_method(Codegen *cg, ASTNode *cls, const char *method) {
    /* Walk the class hierarchy. Limit depth to prevent infinite loops
       on cyclic inheritance (A extends B, B extends A). */
    for (ASTNode *cur = cls, *prev = NULL; cur && cur != prev; ) {
        if (cg_class_has_method(cur, method)) return cur;
        const char *base_name = cg_class_base_name(cur);
        if (!base_name) break;
        prev = cur;
        cur = cg_class_find(cg, base_name);
    }
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
static void gen_block_body(Codegen *cg, ASTNode *n);

/* Infer whether a `+` binary op should be lowered as string concatenation.
   Returns true when either operand is a string literal, a known str-typed
   variable, or itself a string concatenation. */
static bool binop_is_str_concat(Codegen *cg, ASTNode *n) {
    if (!n || n->kind != NODE_EXPR_BINOP) return false;
    ASTNode *l = n->children.count > 0 ? n->children.items[0] : NULL;
    ASTNode *r = n->children.count > 1 ? n->children.items[1] : NULL;
    /* string literal on either side → concat */
    if (l && l->kind == NODE_LITERAL_STR) return true;
    if (r && r->kind == NODE_LITERAL_STR) return true;
    /* f-string on either side → concat */
    if (l && l->kind == NODE_FSTRING) return true;
    if (r && r->kind == NODE_FSTRING) return true;
    /* known str-typed variable on either side → concat */
    if (l && l->kind == NODE_IDENT) {
        Symbol *s = cg_sym_find(cg, l->sval);
        if (s && (!strcmp(s->type, "str") || !strcmp(s->type, "id"))) return true;
    }
    if (r && r->kind == NODE_IDENT) {
        Symbol *s = cg_sym_find(cg, r->sval);
        if (s && (!strcmp(s->type, "str") || !strcmp(s->type, "id"))) return true;
    }
    /* nested string concat → concat */
    if (l && binop_is_str_concat(cg, l)) return true;
    if (r && binop_is_str_concat(cg, r)) return true;
    return false;
}

/* Classify an operand for string-concat purposes:
   returns "str", "int", "float", "bool", or "auto". */
static const char *operand_str_kind(Codegen *cg, ASTNode *n) {
    if (!n) return "auto";
    switch (n->kind) {
    case NODE_LITERAL_STR: return "str";
    case NODE_LITERAL_INT: return "int";
    case NODE_LITERAL_FLOAT: return "float";
    case NODE_LITERAL_BOOL: return "bool";
    case NODE_FSTRING: return "str";
    case NODE_IDENT: {
        Symbol *s = cg_sym_find(cg, n->sval);
        if (!s) return "auto";
        if (!strcmp(s->type, "str") || !strcmp(s->type, "id")) return "str";
        if (!strcmp(s->type, "float")) return "float";
        if (!strcmp(s->type, "bool")) return "bool";
        if (!strcmp(s->type, "int") || !strcmp(s->type, "int64") ||
            !strcmp(s->type, "byte")) return "int";
        return "auto";
    }
    case NODE_EXPR_BINOP:
        if (binop_is_str_concat(cg, n)) return "str";
        return "auto";
    default:
        return "auto";
    }
}

/* Emit an operand of string concatenation, wrapping non-string values
   with the appropriate lu_str_from_X helper. */
static void emit_str_operand(Codegen *cg, ASTNode *n) {
    const char *kind = operand_str_kind(cg, n);
    if (!strcmp(kind, "str") || !strcmp(kind, "auto")) {
        /* str or unknown: emit as-is. C will do the implicit conversion
           from char* to const char*. For unknown types, we hope for the best. */
        gen_expr(cg, n);
    } else if (!strcmp(kind, "int")) {
        emit(cg, "lu_str_from_int((int64_t)(");
        gen_expr(cg, n);
        emit(cg, "))");
    } else if (!strcmp(kind, "float")) {
        emit(cg, "lu_str_from_float((double)(");
        gen_expr(cg, n);
        emit(cg, "))");
    } else if (!strcmp(kind, "bool")) {
        emit(cg, "lu_str_from_bool((bool)(");
        gen_expr(cg, n);
        emit(cg, "))");
    } else {
        gen_expr(cg, n);
    }
}

/* ─────────────────────────────────────────────
   F-string v2: real AST-based expression parsing
   ─────────────────────────────────────────────
   F-strings are now parsed into NODE_FSTRING with children that are
   parts (annot="lit" for literal text, annot="expr" for expressions).
   The expressions are real AST nodes, so codegen can use the same
   type inference as everything else. */

/* Determine the kind ("int", "float", "bool", "str", "auto") of an
   AST expression for f-string conversion. */
static const char *expr_kind_from_ast(Codegen *cg, ASTNode *n) {
    if (!n) return "auto";
    switch (n->kind) {
    case NODE_LITERAL_INT:   return "int";
    case NODE_LITERAL_FLOAT: return "float";
    case NODE_LITERAL_STR:   return "str";
    case NODE_LITERAL_BOOL:  return "bool";
    case NODE_IDENT: {
        /* Handle dotted identifiers like "p.x" that the lexer produces
           as a single token. We look up the base variable and apply
           field-name heuristics. */
        const char *dot = n->sval ? strchr(n->sval, '.') : NULL;
        if (dot && dot != n->sval && dot[1]) {
            /* obj.field — apply field name heuristics */
            const char *field = dot + 1;
            if (!strcmp(field, "read") || !strcmp(field, "online") ||
                !strcmp(field, "is_group") || !strcmp(field, "active") ||
                !strncmp(field, "is_", 3) || !strncmp(field, "has_", 4))
                return "bool";
            if (!strcmp(field, "len") || !strcmp(field, "count") ||
                !strcmp(field, "size") || !strcmp(field, "top") ||
                !strcmp(field, "unread") || !strcmp(field, "cap") ||
                !strcmp(field, "id") || !strcmp(field, "num") ||
                !strcmp(field, "x") || !strcmp(field, "y") ||
                !strcmp(field, "z") || !strcmp(field, "w") ||
                !strcmp(field, "value") || !strcmp(field, "total") ||
                !strcmp(field, "sum") || !strcmp(field, "count"))
                return "int";
            if (!strcmp(field, "name") || !strcmp(field, "text") ||
                !strcmp(field, "sender") || !strcmp(field, "timestamp") ||
                !strcmp(field, "cid") || !strcmp(field, "uid") ||
                !strcmp(field, "data") || !strcmp(field, "meta"))
                return "str";
            /* Default for unknown fields: assume int (most common) */
            return "int";
        }
        Symbol *s = cg_sym_find(cg, n->sval);
        if (!s) return "auto";
        if (!strcmp(s->type, "int") || !strcmp(s->type, "int64") || !strcmp(s->type, "byte")) return "int";
        if (!strcmp(s->type, "float")) return "float";
        if (!strcmp(s->type, "bool")) return "bool";
        if (!strcmp(s->type, "str") || !strcmp(s->type, "id")) return "str";
        return "auto";
    }
    case NODE_EXPR_BINOP: {
        const char *op = n->op ? n->op : "";
        if (!strcmp(op, "==") || !strcmp(op, "!=") || !strcmp(op, "<") ||
            !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=") ||
            !strcmp(op, "&&") || !strcmp(op, "||"))
            return "bool";
        if (!strcmp(op, "**")) return "float";
        if (!strcmp(op, "+")) {
            const char *lt = expr_kind_from_ast(cg, n->children.count > 0 ? n->children.items[0] : NULL);
            const char *rt = expr_kind_from_ast(cg, n->children.count > 1 ? n->children.items[1] : NULL);
            if (!strcmp(lt, "str") || !strcmp(rt, "str")) return "str";
        }
        const char *lt = expr_kind_from_ast(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        const char *rt = expr_kind_from_ast(cg, n->children.count > 1 ? n->children.items[1] : NULL);
        if (!strcmp(lt, "float") || !strcmp(rt, "float")) return "float";
        return "int";
    }
    case NODE_EXPR_UNOP:
        return expr_kind_from_ast(cg, n->children.count > 0 ? n->children.items[0] : NULL);
    case NODE_FUNC_CALL: {
        if (n->sval) {
            const char *dot = strchr(n->sval, '.');
            if (dot) {
                char obj[128];
                size_t obj_len = (size_t)(dot - n->sval);
                if (obj_len >= sizeof(obj)) obj_len = sizeof(obj) - 1;
                memcpy(obj, n->sval, obj_len);
                obj[obj_len] = '\0';
                Symbol *os = cg_sym_find(cg, obj);
                if (os) {
                    /* Try direct method lookup first. */
                    char mangled[256];
                    snprintf(mangled, sizeof(mangled), "%s_%s", os->type, dot + 1);
                    Symbol *ms = cg_sym_find(cg, mangled);
                    /* If not found, try inherited methods by walking the
                       class hierarchy. */
                    if (!ms) {
                        ASTNode *cls = cg_class_find(cg, os->type);
                        if (cls) {
                            ASTNode *declared_in = cg_class_resolve_method(cg, cls, dot + 1);
                            if (declared_in) {
                                snprintf(mangled, sizeof(mangled), "%s_%s",
                                         declared_in->sval, dot + 1);
                                ms = cg_sym_find(cg, mangled);
                            }
                        }
                    }
                    if (ms) {
                        if (!strcmp(ms->type, "str") || !strcmp(ms->type, "id")) return "str";
                        if (!strcmp(ms->type, "int") || !strcmp(ms->type, "int64") || !strcmp(ms->type, "byte")) return "int";
                        if (!strcmp(ms->type, "float")) return "float";
                        if (!strcmp(ms->type, "bool")) return "bool";
                    }
                    /* Vector<T>.len() / .get() / .pop() return int */
                    if (!strncmp(os->type, "Vector<", 7)) {
                        if (!strcmp(dot + 1, "len") || !strcmp(dot + 1, "get") ||
                            !strcmp(dot + 1, "pop"))
                            return "int";
                    }
                    /* "init" / "speak" / "name" etc. heuristics for inherited */
                    if (!strcmp(dot + 1, "init")) return "void";
                    if (!strcmp(dot + 1, "speak") || !strcmp(dot + 1, "to_string"))
                        return "str";
                }
            } else {
                /* Built-in functions with known return types */
                if (!strcmp(n->sval, "len") || !strcmp(n->sval, "min") ||
                    !strcmp(n->sval, "max") || !strcmp(n->sval, "abs") ||
                    !strcmp(n->sval, "sqrt") || !strcmp(n->sval, "floor") ||
                    !strcmp(n->sval, "ceil") || !strcmp(n->sval, "round") ||
                    !strcmp(n->sval, "pow") || !strcmp(n->sval, "sin") ||
                    !strcmp(n->sval, "cos") || !strcmp(n->sval, "tan"))
                    return !strcmp(n->sval, "len") || !strcmp(n->sval, "min") ||
                           !strcmp(n->sval, "max") || !strcmp(n->sval, "abs") ? "int" : "float";
                if (!strcmp(n->sval, "upper") || !strcmp(n->sval, "lower") ||
                    !strcmp(n->sval, "replace") || !strcmp(n->sval, "read_file") ||
                    !strcmp(n->sval, "input"))
                    return "str";
                if (!strcmp(n->sval, "contains"))
                    return "bool";
                /* User-defined function: look up in symbol table */
                Symbol *s = cg_sym_find(cg, n->sval);
                if (s) {
                    if (!strcmp(s->type, "str") || !strcmp(s->type, "id")) return "str";
                    if (!strcmp(s->type, "int") || !strcmp(s->type, "int64") || !strcmp(s->type, "byte")) return "int";
                    if (!strcmp(s->type, "float")) return "float";
                    if (!strcmp(s->type, "bool")) return "bool";
                }
            }
        }
        return "auto";
    }
    case NODE_EXPR_FIELD: {
        if (n->sval) {
            const char *field = n->sval;
            if (!strcmp(field, "read") || !strcmp(field, "online") ||
                !strcmp(field, "is_group") || !strcmp(field, "active") ||
                !strncmp(field, "is_", 3) || !strncmp(field, "has_", 4))
                return "bool";
            if (!strcmp(field, "len") || !strcmp(field, "count") ||
                !strcmp(field, "size") || !strcmp(field, "top") ||
                !strcmp(field, "unread") || !strcmp(field, "cap") ||
                !strcmp(field, "id") || !strcmp(field, "num"))
                return "int";
            if (!strcmp(field, "name") || !strcmp(field, "text") ||
                !strcmp(field, "sender") || !strcmp(field, "timestamp") ||
                !strcmp(field, "cid") || !strcmp(field, "uid") ||
                !strcmp(field, "data") || !strcmp(field, "meta"))
                return "str";
        }
        return "auto";
    }
    case NODE_EXPR_INDEX: {
        /* arr[i] — the result type is the element type of the array.
           We look up the base identifier's type (which is the element
           type, since that's how array vars are registered). This fixes
           the segfault where `f"{arr[i]}"` on an int array was emitted
           as printf("%s", arr[i]) — treating the int as a char*. */
        ASTNode *base = n->children.count > 0 ? n->children.items[0] : NULL;
        /* List literal [1,2,3][i] → int (compound literal of int) */
        if (base && base->kind == NODE_EXPR_INDEX && base->op &&
            !strcmp(base->op, "list")) {
            return "int";
        }
        /* Nested indexing: matrix[i][j] — recurse on the base */
        if (base && base->kind == NODE_EXPR_INDEX) {
            return expr_kind_from_ast(cg, base);
        }
        /* Regular array: look up the base identifier */
        if (base && base->kind == NODE_IDENT) {
            /* Handle dotted identifiers like "this.data[i]" */
            const char *dot = strchr(base->sval, '.');
            if (dot && dot != base->sval && dot[1]) {
                const char *field = dot + 1;
                /* Common int array field names */
                if (!strcmp(field, "data") || !strcmp(field, "counts") ||
                    !strcmp(field, "arr") || !strcmp(field, "values"))
                    return "int";
                return "int"; /* default for unknown array fields */
            }
            Symbol *s = cg_sym_find(cg, base->sval);
            if (s) {
                if (!strcmp(s->type, "int") || !strcmp(s->type, "int64") || !strcmp(s->type, "byte")) return "int";
                if (!strcmp(s->type, "float")) return "float";
                if (!strcmp(s->type, "bool")) return "bool";
                if (!strcmp(s->type, "str") || !strcmp(s->type, "id")) return "str";
            }
        }
        /* Safe default: assume int (most common array element type).
           This prevents the segfault — int→str conversion is always safe. */
        return "int";
    }
    case NODE_EXPR_TERNARY: {
        /* cond ? a : b — infer from the "then" branch (a).
           Both branches should have compatible types. */
        return expr_kind_from_ast(cg, n->children.count > 1 ? n->children.items[1] : NULL);
    }
    case NODE_NEW_EXPR:
        /* new Type() returns Type* — treat as pointer (str for f-string) */
        return "str";
    case NODE_EXPR_DEREF: {
        /* *p — assume int for now (could be improved by tracking pointer types) */
        return "int";
    }
    case NODE_EXPR_REF:
        /* &x — returns a pointer, treat as str (char*) */
        return "str";
    case NODE_FSTRING:
        return "str";
    default:
        return "auto";
    }
}

/* Emit a value as a string for f-string concatenation, wrapping it
   with the appropriate conversion helper based on its inferred type. */
static void emit_fstring_expr(Codegen *cg, ASTNode *n) {
    const char *kind = expr_kind_from_ast(cg, n);
    if (!strcmp(kind, "int")) {
        emit(cg, "lu_str_from_int((int64_t)(");
        gen_expr(cg, n);
        emit(cg, "))");
    } else if (!strcmp(kind, "float")) {
        emit(cg, "lu_str_from_float((double)(");
        gen_expr(cg, n);
        emit(cg, "))");
    } else if (!strcmp(kind, "bool")) {
        emit(cg, "lu_str_from_bool((bool)(");
        gen_expr(cg, n);
        emit(cg, "))");
    } else {
        /* str or auto: emit as-is (assume char*) */
        gen_expr(cg, n);
    }
}

/* Emit a NODE_FSTRING as a series of lu_str_concat calls.
   The f-string's children are parts with annot="lit" (literal text)
   or annot="expr" (parsed expression in children[0]). */
static void emit_fstring_node(Codegen *cg, ASTNode *n) {
    int nparts = n->children.count;
    if (nparts == 0) {
        emit(cg, "\"\"");
        return;
    }
    if (nparts == 1) {
        ASTNode *p = n->children.items[0];
        if (p->annot && !strcmp(p->annot, "expr") && p->children.count > 0) {
            emit_fstring_expr(cg, p->children.items[0]);
        } else {
            emit_c_string_literal(cg, p->sval ? p->sval : "");
        }
        return;
    }
    /* Nest from left: lu_str_concat(p0, lu_str_concat(p1, ... p(n-1))) */
    for (int k = 0; k < nparts - 1; k++) {
        emit(cg, "lu_str_concat(");
        ASTNode *p = n->children.items[k];
        if (p->annot && !strcmp(p->annot, "expr") && p->children.count > 0) {
            emit_fstring_expr(cg, p->children.items[0]);
        } else {
            emit_c_string_literal(cg, p->sval ? p->sval : "");
        }
        emit(cg, ", ");
    }
    /* Last part */
    {
        ASTNode *p = n->children.items[nparts - 1];
        if (p->annot && !strcmp(p->annot, "expr") && p->children.count > 0) {
            emit_fstring_expr(cg, p->children.items[0]);
        } else {
            emit_c_string_literal(cg, p->sval ? p->sval : "");
        }
    }
    for (int k = 0; k < nparts - 1; k++) {
        emit(cg, ")");
    }
}

/* Infer the C type of an expression for `auto` declarations. */
static const char *infer_c_type(Codegen *cg, ASTNode *n) {
    if (!n) return "int";
    switch (n->kind) {
    case NODE_LITERAL_INT:   return "int";
    case NODE_LITERAL_FLOAT: return "double";
    case NODE_LITERAL_STR:   return "char*";
    case NODE_LITERAL_BOOL:  return "bool";
    case NODE_FSTRING:       return "char*";
    case NODE_IDENT: {
        Symbol *s = cg_sym_find(cg, n->sval);
        if (!s) return "int";
        return lu2c(s->type);
    }
    case NODE_EXPR_BINOP: {
        /* String concat → char* */
        if (n->op && !strcmp(n->op, "+") && binop_is_str_concat(cg, n))
            return "char*";
        /* Comparison → bool */
        const char *op = n->op ? n->op : "";
        if (!strcmp(op,"==") || !strcmp(op,"!=") || !strcmp(op,"<") ||
            !strcmp(op,">")  || !strcmp(op,"<=") || !strcmp(op,">=") ||
            !strcmp(op,"&&") || !strcmp(op,"||"))
            return "bool";
        /* ** → double */
        if (!strcmp(op, "**")) return "double";
        /* Numeric: prefer wider */
        const char *lt = infer_c_type(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        const char *rt = infer_c_type(cg, n->children.count > 1 ? n->children.items[1] : NULL);
        if (!strcmp(lt, "double") || !strcmp(rt, "double")) return "double";
        return lt;
    }
    case NODE_NEW_EXPR:
        /* new Type() → Type* */
        return n->sval ? n->sval : "void*";
    case NODE_FUNC_CALL: {
        Symbol *s = cg_sym_find(cg, n->sval);
        if (s) return lu2c(s->type);
        return "int";
    }
    case NODE_EXPR_INDEX:
        /* list literal [1,2,3] → int* (compound literal decays) */
        if (n->op && !strcmp(n->op, "list")) return "int*";
        /* slice arr[a:b] → lu_slice_t */
        if (n->op && !strcmp(n->op, "slice")) return "lu_slice_t";
        return "int";
    default:
        return "int";
    }
}

/* ─────────────────────────────────────────────
   Expression emitter
   ───────────────────────────────────────────── */
static void gen_expr(Codegen *cg, ASTNode *n) {
    if (!n) { emit(cg, "0"); return; }
    switch (n->kind) {
    case NODE_LITERAL_INT:
        /* null literal — emit NULL */
        if (n->sval && !strcmp(n->sval, "null")) {
            emit(cg, "NULL");
            break;
        }
        emit(cg, "%lld", (long long)n->ival);
        break;
    case NODE_LITERAL_FLOAT: emit(cg, "%g", n->fval); break;
    case NODE_LITERAL_BOOL:  emit(cg, "%s", n->bval ? "true" : "false"); break;
    case NODE_LITERAL_STR: {
        const char *v = n->sval ? n->sval : "";
        /* f-string literal: $f... (legacy fallback — normally f-strings
           are parsed into NODE_FSTRING by parse_fstring). */
        if (v[0] == '$' && v[1] == 'f') {
            /* This should not happen with v2, but handle it as a plain string */
            emit_c_string_literal(cg, v + 2);
            break;
        }
        if (v[0] == '$') { emit(cg, "/* template */ "); emit_c_string_literal(cg, v + 1); break; }
        emit_c_string_literal(cg, v);
        break;
    }
    case NODE_FSTRING:
        emit_fstring_node(cg, n);
        break;
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
        } else if (n->op && !strcmp(n->op, "+") &&
                   binop_is_str_concat(cg, n)) {
            /* String concatenation. Wrap non-string operands with the
               appropriate conversion helper so users can write
               "Count: " + 42 without manual conversion. */
            emit(cg, "lu_str_concat(");
            emit_str_operand(cg, n->children.count > 0 ? n->children.items[0] : NULL);
            emit(cg, ", ");
            emit_str_operand(cg, n->children.count > 1 ? n->children.items[1] : NULL);
            emit(cg, ")");
        } else {
            /* Operator overloading: if the left operand is a class instance
               and the class declares op<symbol>, lower to Class_op_mangled(a, b). */
            ASTNode *left = n->children.count > 0 ? n->children.items[0] : NULL;
            bool used_op_overload = false;
            if (left && n->op) {
                /* Determine the left operand's class. */
                const char *cls_name = NULL;
                if (left->kind == NODE_IDENT) {
                    Symbol *s = cg_sym_find(cg, left->sval);
                    if (s) cls_name = s->type;
                }
                if (cls_name) {
                    ASTNode *cls = cg_class_find(cg, cls_name);
                    if (cls) {
                        /* Look for an op overload with this operator. */
                        const char *mangled = op_mangle(n->op);
                        for (int i = 0; i < cls->children.count; i++) {
                            ASTNode *m = cls->children.items[i];
                            if (m->kind == NODE_OP_OVERLOAD && m->op) {
                                const char *m_mangled = op_mangle(m->op);
                                if (!strcmp(mangled, m_mangled)) {
                                    /* Found: emit Class_op_mangled(&a, b) */
                                    emit(cg, "%s_op_%s(", cls_name, mangled);
                                    emit(cg, "&");
                                    gen_expr(cg, left);
                                    emit(cg, ", ");
                                    gen_expr(cg, n->children.count > 1 ? n->children.items[1] : NULL);
                                    emit(cg, ")");
                                    used_op_overload = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (!used_op_overload) {
                emit(cg, "(");
                gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
                emit(cg, " %s ", n->op ? n->op : "+");
                gen_expr(cg, n->children.count > 1 ? n->children.items[1] : NULL);
                emit(cg, ")");
            }
        }
        break;
    case NODE_EXPR_UNOP: {
        const char *op = n->op ? n->op : "!";
        /* Type cast: op = "(cast:type)" → emit ((type)(expr)) */
        if (!strncmp(op, "(cast:", 6)) {
            const char *tname = op + 6;
            /* strip trailing ')' */
            char tbuf[64];
            int tlen = (int)strlen(tname);
            if (tlen > 0 && tname[tlen-1] == ')') tlen--;
            if (tlen >= (int)sizeof(tbuf)) tlen = sizeof(tbuf) - 1;
            memcpy(tbuf, tname, tlen);
            tbuf[tlen] = '\0';
            emit(cg, "((%s)(", lu2c(tbuf));
            gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
            emit(cg, "))");
            break;
        }
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
        /* List literal: [1, 2, 3] → (int[]){1, 2, 3} (C99 compound literal) */
        if (n->op && !strcmp(n->op, "list")) {
            /* Determine element type from first element */
            const char *elem_type = "int";
            if (n->children.count > 0) {
                elem_type = infer_c_type(cg, n->children.items[0]);
                if (!strcmp(elem_type, "char*")) elem_type = "char*";
                else if (!strcmp(elem_type, "double")) elem_type = "double";
                else if (!strcmp(elem_type, "bool")) elem_type = "bool";
                else elem_type = "int";
            }
            emit(cg, "((%s[]){", elem_type);
            for (int i = 0; i < n->children.count; i++) {
                if (i) emit(cg, ", ");
                gen_expr(cg, n->children.items[i]);
            }
            if (n->children.count == 0) emit(cg, "0");
            emit(cg, "})");
            break;
        }
        /* Slice: arr[start:end] → lowered to lu_slice_t { ptr, len }
           as a compound literal. Use char* for pointer arithmetic to
           avoid -Wpointer-arith on void*. */
        if (n->op && !strcmp(n->op, "slice")) {
            ASTNode *arr = n->children.count > 0 ? n->children.items[0] : NULL;
            ASTNode *start = n->children.count > 1 ? n->children.items[1] : NULL;
            ASTNode *end = n->children.count > 2 ? n->children.items[2] : NULL;
            emit(cg, "((lu_slice_t){ (void*)((char*)(");
            gen_expr(cg, arr);
            emit(cg, ") + (");
            gen_expr(cg, start);
            emit(cg, ") * sizeof(*(");
            gen_expr(cg, arr);
            emit(cg, "))), (int)(");
            gen_expr(cg, end);
            emit(cg, ") - (int)(");
            gen_expr(cg, start);
            emit(cg, ") })");
            break;
        }
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

            /* super.method() — call the base class's method on this->base. */
            if (!strcmp(obj, "super") && cg->current_class) {
                const char *base_name = cg_class_base_name(cg->current_class);
                if (base_name) {
                    emit(cg, "%s_%s(", base_name, method);
                    emit(cg, "&this->base");
                    for (int i = 0; i < n->children.count; i++) {
                        emit(cg, ", ");
                        gen_expr(cg, n->children.items[i]);
                    }
                    emit(cg, ")");
                    break;
                }
            }

            Symbol *sym = cg_sym_find(cg, obj);
            const char *ty = sym ? sym->type : obj;

            /* Method resolution with inheritance.
               If the object's class doesn't declare the method directly,
               walk up the class hierarchy to find the base class that does,
               and emit Base_method(&obj.base, ...) (or Base_method(obj, ...)
               for pointers — the base field is at offset 0 so the cast is
               valid). */
            const char *emit_class = ty;       /* class name to prefix the method */
            bool need_base_cast = false;       /* if true, emit &obj.base */

            ASTNode *cls = cg_class_find(cg, ty);
            if (cls) {
                ASTNode *declared_in = cg_class_resolve_method(cg, cls, method);
                if (declared_in && declared_in != cls) {
                    /* Method is inherited — call Base_method on the base subobject. */
                    emit_class = declared_in->sval;
                    need_base_cast = true;
                }
            }

            /* For Vector<T> types, the C type is lu_vector_<T>, and methods
               are lu_vector_<T>_method. */
            if (!strncmp(emit_class, "Vector<", 7)) {
                const char *c_ty = lu2c(emit_class);
                emit(cg, "%s_%s(", c_ty, method);
            } else {
                emit(cg, "%s_%s(", emit_class, method);
            }
            /* Emit the object argument. For inherited methods, we need
               to pass the base subobject: &obj.base (or obj for pointers,
               since base is at offset 0 the cast is implicit). */
            if (need_base_cast) {
                if (sym && sym->is_ptr) {
                    /* ptr/Base → cast pointer (base is at offset 0) */
                    emit(cg, "(%s *)(%s)", emit_class, obj);
                } else {
                    /* value: pass &obj.base */
                    emit(cg, "&(%s).base", obj);
                }
            } else {
                if (sym && sym->is_ptr) emit(cg, "%s", obj);
                else emit(cg, "&%s", obj);
            }
            for (int i = 0; i < n->children.count; i++) {
                emit(cg, ", ");
                gen_expr(cg, n->children.items[i]);
            }
            emit(cg, ")");
        } else {
            /* Remap built-in function names to their C runtime equivalents. */
            const char *emit_name = name;
            if (!strcmp(name, "min")) emit_name = "lu_min";
            else if (!strcmp(name, "max")) emit_name = "lu_max";
            else if (!strcmp(name, "len")) emit_name = "lu_str_len";
            else if (!strcmp(name, "upper")) emit_name = "lu_str_upper";
            else if (!strcmp(name, "lower")) emit_name = "lu_str_lower";
            else if (!strcmp(name, "contains")) emit_name = "lu_str_contains";
            else if (!strcmp(name, "replace")) emit_name = "lu_str_replace";
            else if (!strcmp(name, "read_file")) emit_name = "lu_read_file";
            else if (!strcmp(name, "write_file")) emit_name = "lu_write_file";
            else if (!strcmp(name, "input")) emit_name = "lu_input";
            emit(cg, "%s(", emit_name);
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
        /* new Type() — allocate. new Type(args) — allocate + init for primitives.
           Use lu2c() to translate Lu type names to C (str→char*, float→double, etc.). */
        if (n->children.count > 0) {
            const char *c_type = lu2c(n->sval);
            emit(cg, "({ %s *_t = (%s*)malloc(sizeof(%s)); *_t = (", c_type, c_type, c_type);
            gen_expr(cg, n->children.items[0]);
            emit(cg, "); _t; })");
        } else {
            emit(cg, "calloc(1, sizeof(%s))", lu2c(n->sval ? n->sval : "char"));
        }
        break;
    case NODE_FUNC_DECL:
        /* Lambda expression: the function was already emitted by
           emit_lambdas() pre-pass. Just emit its address. */
        if (n->annot && !strcmp(n->annot, "lambda")) {
            emit(cg, "&%s", n->op ? n->op : "_lu_lambda_0");
            break;
        }
        /* fall through if not a lambda */
        /* fallthrough */
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
            emit_c_decl(cg, c->type_name, c->sval ? c->sval : "_p");
            /* Register the parameter so the codegen knows its type when
               deciding things like string-concat vs integer-add. */
            cg_sym_add(cg, c->sval ? c->sval : "_p", c->type_name, false);
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

/* Register function parameters (and `this`) in the codegen symbol table.
   Called only from definition emission, not prototype emission, so that
   prototype parameter names don't pollute the global symbol table. */
static void register_fn_params(Codegen *cg, ASTNode *fn, const char *self_type) {
    if (self_type && *self_type) {
        cg_sym_add(cg, "this", self_type, true);
    }
    for (int i = 0; fn && i < fn->children.count; i++) {
        ASTNode *c = fn->children.items[i];
        if (c->kind != NODE_VAR_DECL) break;
        if (c->sval) cg_sym_add(cg, c->sval, c->type_name, false);
    }
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
    /* Register the function's parameters (and `this` for methods) in the
       codegen symbol table before emitting the body, so type-aware codegen
       like string-concat detection works inside the body. */
    register_fn_params(cg, fn, self_type);
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
        if (f->kind == NODE_VAR_DECL) {
            int fndims = (int)f->ival;
            bool is_array = (f->op && !strcmp(f->op, "array")) || fndims > 0;
            if (is_array && fndims > 0) {
                iemit(cg, "%s %s", class_field_c_type(cls, f->type_name), f->sval ? f->sval : "_field");
                for (int d = 0; d < fndims && d < f->children.count; d++) {
                    emit(cg, "[");
                    gen_expr(cg, f->children.items[d]);
                    emit(cg, "]");
                }
                emit(cg, ";\n");
            } else {
                iemit(cg, "%s %s;\n", class_field_c_type(cls, f->type_name), f->sval ? f->sval : "_field");
            }
        }
    }

    cg->indent--;
    emit(cg, "};\n");
}

/* emit_param_list_for_class: like emit_param_list, but uses class_method_c_type
   so template<T> method parameters of type T are lowered to void*. */
static int emit_param_list_for_class(Codegen *cg, ASTNode *fn, const char *self_type, ASTNode *cls) {
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
            emit_c_decl(cg, class_method_c_type(cls, c->type_name), c->sval ? c->sval : "_p");
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

static void emit_class_method_signature(Codegen *cg, ASTNode *fn, const char *name, const char *self_type, ASTNode *cls) {
    emit(cg, "%s %s(", class_method_c_type(cls, fn ? fn->type_name : "void"), name ? name : "_fn");
    emit_param_list_for_class(cg, fn, self_type, cls);
    emit(cg, ")");
}

static void gen_class_method_definition(Codegen *cg, ASTNode *fn, const char *name, const char *self_type, ASTNode *cls) {
    int body_start;
    emit(cg, "\n%s %s(", class_method_c_type(cls, fn ? fn->type_name : "void"), name ? name : "_fn");
    body_start = emit_param_list_for_class(cg, fn, self_type, cls);
    emit(cg, ") {\n");
    /* Register `this` and params in the codegen symbol table before
       emitting the body. For template classes, params of type T are
       lowered to void*. */
    if (self_type && *self_type) cg_sym_add(cg, "this", self_type, true);
    for (int i = 0; fn && i < fn->children.count; i++) {
        ASTNode *c = fn->children.items[i];
        if (c->kind != NODE_VAR_DECL) break;
        if (c->sval) {
            const char *ty = c->type_name;
            /* If this is a template class and the param type is T, use void*. */
            if (cls && cls->annot && ty && !strcmp(cls->annot, ty)) ty = "ptr";
            cg_sym_add(cg, c->sval, ty, !strcmp(ty, "ptr"));
        }
    }
    /* Set current_class so super.method() can resolve to Base_method. */
    ASTNode *saved_class = cg->current_class;
    cg->current_class = cls;
    cg->indent++;
    for (int i = body_start; fn && i < fn->children.count; i++)
        gen_node(cg, fn->children.items[i]);
    cg->indent--;
    cg->current_class = saved_class;
    emit(cg, "}\n");
}

static void gen_class_method_prototypes(Codegen *cg, ASTNode *cls) {
    const char *cn = cls && cls->sval ? cls->sval : "LuClass";
    for (int i = 0; cls && i < cls->children.count; i++) {
        ASTNode *m = cls->children.items[i];
        char name[256];
        if (m->kind == NODE_METHOD_DECL) {
            snprintf(name, sizeof(name), "%s_%s", cn, m->sval ? m->sval : "method");
            emit_class_method_signature(cg, m, name, cn, cls);
            emit(cg, ";\n");
        } else if (m->kind == NODE_CONSTRUCTOR) {
            snprintf(name, sizeof(name), "%s_init", cn);
            emit(cg, "void %s(", name);
            emit_param_list_for_class(cg, m, cn, cls);
            emit(cg, ");\n");
        } else if (m->kind == NODE_DESTRUCTOR) {
            snprintf(name, sizeof(name), "%s_deinit", cn);
            emit(cg, "void %s(%s *this);\n", name, cn);
        } else if (m->kind == NODE_OP_OVERLOAD) {
            snprintf(name, sizeof(name), "%s_op_%s", cn, op_mangle(m->op));
            emit_class_method_signature(cg, m, name, cn, cls);
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
            gen_class_method_definition(cg, m, name, cn, cls);
        } else if (m->kind == NODE_CONSTRUCTOR) {
            snprintf(name, sizeof(name), "%s_init", cn);
            m->type_name = m->type_name ? m->type_name : lu_strdup("void");
            gen_class_method_definition(cg, m, name, cn, cls);
        } else if (m->kind == NODE_DESTRUCTOR) {
            snprintf(name, sizeof(name), "%s_deinit", cn);
            m->type_name = m->type_name ? m->type_name : lu_strdup("void");
            gen_class_method_definition(cg, m, name, cn, cls);
        } else if (m->kind == NODE_OP_OVERLOAD) {
            snprintf(name, sizeof(name), "%s_op_%s", cn, op_mangle(m->op));
            gen_class_method_definition(cg, m, name, cn, cls);
        }
    }
}

static bool top_level_skip_after_preamble(ASTNode *n) {
    if (!n) return true;
    switch (n->kind) {
    case NODE_LANG_DECL: case NODE_IMPORT: case NODE_MODE: case NODE_OPT:
    case NODE_ANNOT: case NODE_MODULE: case NODE_DEFINE:
    case NODE_DEF_CONST: case NODE_DEF_CONFIG:
    case NODE_STRUCT_DECL: case NODE_UNION_DECL: case NODE_ENUM_DECL:
    case NODE_FUNC_DECL: case NODE_ASYNC_FUNC:
    case NODE_VAR_DECL:  /* emitted as globals in pre-pass */
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

    /* Global variable declarations (top-level NODE_VAR_DECL without initialiser
       or with constant initialiser) — emitted before functions so functions
       can reference them. */
    for (int i = 0; root && i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (n->kind == NODE_VAR_DECL) {
            /* Emit as a file-scope static variable with optional initialiser. */
            const char *t = lu2c(n->type_name);
            int gndims = (int)n->ival;
            bool is_array = (n->op && !strcmp(n->op, "array")) || gndims > 0;
            if (n->type_name && !strcmp(n->type_name, "auto") && n->children.count > 0 && !is_array) {
                t = infer_c_type(cg, n->children.items[0]);
            }
            /* Use emit_c_decl for function pointer globals, but prefix
               with 'static'. For non-fn types, emit directly. */
            if (t && !strncmp(t, "fn:", 3)) {
                emit(cg, "static ");
                emit_c_decl(cg, t, n->sval ? n->sval : "_g");
            } else {
                emit(cg, "static %s %s", t, n->sval ? n->sval : "_g");
            }
            if (is_array && gndims > 0) {
                /* Multi-dim array global: emit sizes, then optional init */
                for (int d = 0; d < gndims && d < n->children.count; d++) {
                    emit(cg, "[");
                    gen_expr(cg, n->children.items[d]);
                    emit(cg, "]");
                }
                if (n->children.count > gndims) {
                    emit(cg, " = {");
                    for (int j = gndims; j < n->children.count; j++) {
                        if (j > gndims) emit(cg, ", ");
                        gen_expr(cg, n->children.items[j]);
                    }
                    emit(cg, "}");
                }
            } else if (n->children.count > 0 && !is_array) {
                emit(cg, " = ");
                gen_expr(cg, n->children.items[0]);
            }
            emit(cg, ";\n");
            cg_sym_add(cg, n->sval ? n->sval : "_g", n->type_name, false);
        }
    }

    /* Types before prototypes and blocks. */
    for (int i = 0; root && i < root->children.count; i++) {
        ASTNode *n = root->children.items[i];
        if (n->kind == NODE_STRUCT_DECL || n->kind == NODE_UNION_DECL || n->kind == NODE_ENUM_DECL)
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
        /* auto type inference: auto x = expr → infer type from expr.
           We need the Lu type for the symbol table (so expr_kind_from_ast
           can look it up). infer_c_type returns C types, so we map back. */
        const char *inferred_type = n->type_name;
        if (n->type_name && !strcmp(n->type_name, "auto")) {
            if (n->children.count > 0) {
                ASTNode *init = n->children.items[0];
                const char *c_t = infer_c_type(cg, init);
                /* Map C type back to Lu type for the symbol table. */
                if (!strcmp(c_t, "double")) inferred_type = "float";
                else if (!strcmp(c_t, "char*")) inferred_type = "str";
                else if (!strcmp(c_t, "bool")) inferred_type = "bool";
                else if (!strncmp(c_t, "lu_slice_t", 10)) inferred_type = "lu_slice_t";
                else inferred_type = "int";
            } else {
                inferred_type = "int";
            }
        }
        cg_sym_add(cg, n->sval ? n->sval : "_var", inferred_type, false);
        /* Use emit_c_decl so function pointer types put the name inside
           the parens. For auto, use the Lu type (so fn:... works);
           for explicit types, use lu2c of the original type_name. */
        indent(cg);
        if (n->type_name && !strcmp(n->type_name, "auto")) {
            emit_c_decl(cg, inferred_type, n->sval ? n->sval : "_var");
        } else {
            emit_c_decl(cg, n->type_name, n->sval ? n->sval : "_var");
        }
        /* n->ival holds the number of array dimensions (0 = not an array).
           The first n->ival children are the dimension sizes; any remaining
           children are initialiser values. */
        int ndims = (int)n->ival;
        bool is_array = (n->op && !strcmp(n->op, "array")) || ndims > 0;
        if (is_array && ndims > 0) {
            int n_init = n->children.count - ndims;  /* number of init values */
            bool has_init = n_init > 0;
            /* If we have an initialiser and the first dim is not a literal,
               C can't size a VLA from an initialiser — emit [] for that dim. */
            for (int d = 0; d < ndims; d++) {
                ASTNode *dim = n->children.items[d];
                bool literal = dim && dim->kind == NODE_LITERAL_INT;
                if (has_init && !literal && d == 0) {
                    emit(cg, "[]");
                } else {
                    emit(cg, "[");
                    gen_expr(cg, dim);
                    emit(cg, "]");
                }
            }
            if (has_init) {
                emit(cg, " = {");
                for (int i = ndims; i < n->children.count; i++) {
                    if (i > ndims) emit(cg, ", ");
                    gen_expr(cg, n->children.items[i]);
                }
                emit(cg, "}");
            }
        } else if (n->children.count > 0 && !is_array) {
            /* Non-array: children[0] is the initialiser. */
            emit(cg, " = ");
            gen_expr(cg, n->children.items[0]);
        } else if (inferred_type &&
                   (!strncmp(inferred_type, "Unique<", 7) ||
                    !strncmp(inferred_type, "Shared<", 7))) {
            /* Smart pointers must be initialized to NULL for safe cleanup */
            emit(cg, " = {0}");
        }
        emit(cg, ";\n");
        /* Auto-initialize Vector<T> variables */
        if (inferred_type && !strncmp(inferred_type, "Vector<", 7) && n->children.count == 0) {
            const char *vec_c_type = lu2c(inferred_type);
            iemit(cg, "%s_init(&%s);\n", vec_c_type, n->sval ? n->sval : "_var");
        }
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
        ASTNode *item = n->children.count > 0 ? n->children.items[0] : NULL;
        ASTNode *list = n->children.count > 1 ? n->children.items[1] : NULL;
        int li = cg->label_counter++;

        /* Special case: for x in range(N) → for (x=0; x<N; x++) */
        if (list && list->kind == NODE_FUNC_CALL && list->sval &&
            !strcmp(list->sval, "range")) {
            ASTNode *arg = list->children.count > 0 ? list->children.items[0] : NULL;
            iemit(cg, "for (int %s = 0; %s < (int)(", item ? item->sval : "_i",
                  item ? item->sval : "_i");
            gen_expr(cg, arg);
            emit(cg, "); %s++) {\n", item ? item->sval : "_i");
            /* Register the loop variable so f-strings inside the body
               know its type. */
            if (item && item->sval) cg_sym_add(cg, item->sval, "int", false);
            cg->indent++;
            if (n->children.count > 2) gen_block_body(cg, n->children.items[2]);
            cg->indent--;
            iemit(cg, "}\n");
            return;
        }

        /* General case: iterate over array or list literal */
        iemit(cg, "/* Loop/Each */\n");
        /* If list is a list literal, we need a temporary array */
        bool is_list_literal = (list && list->kind == NODE_EXPR_INDEX &&
                                list->op && !strcmp(list->op, "list"));
        if (is_list_literal) {
            /* Determine element type from first element */
            const char *elem_type = "int";
            if (list->children.count > 0) {
                const char *t = infer_c_type(cg, list->children.items[0]);
                if (!strcmp(t, "char*")) elem_type = "char*";
                else if (!strcmp(t, "double")) elem_type = "double";
                else if (!strcmp(t, "bool")) elem_type = "bool";
                else elem_type = "int";
            }
            iemit(cg, "{ %s _tmp%d[] = {", elem_type, li);
            for (int i = 0; i < list->children.count; i++) {
                if (i) emit(cg, ", ");
                gen_expr(cg, list->children.items[i]);
            }
            if (list->children.count == 0) emit(cg, "0");
            emit(cg, "};\n");
            iemit(cg, "for (int _ei%d = 0; _ei%d < (int)(sizeof(_tmp%d)/sizeof(_tmp%d[0])); _ei%d++) {\n",
                  li, li, li, li, li);
            cg->indent++;
            iemit(cg, "%s %s = _tmp%d[_ei%d];\n", elem_type, item ? item->sval : "_item", li, li);
            if (n->children.count > 2) gen_block_body(cg, n->children.items[2]);
            cg->indent--;
            iemit(cg, "}\n");
            iemit(cg, "}\n");
        } else {
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
        }
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
        /* f-string via NODE_FSTRING (v2) */
        if (e->kind == NODE_FSTRING) {
            iemit(cg, "printf(\"%%s\\n\", ");
            emit_fstring_node(cg, e);
            emit(cg, ");\n");
            return;
        }
        if (e->kind == NODE_LITERAL_STR) {
            const char *v = e->sval ? e->sval : "";
            /* Legacy $f marker (shouldn't happen with v2 parser, but keep
               a fallback that just prints the raw content as a string). */
            if (v[0] == '$' && v[1] == 'f') {
                iemit(cg, "printf(\"%%s\\n\", ");
                emit_c_string_literal(cg, v + 2);
                emit(cg, ");\n");
                return;
            }
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

    /* ── Set/ → assignment (plain or compound) ── */
    case NODE_SET: {
        indent(cg);
        emit_c_ident(cg, n->sval ? n->sval : "_var");
        if (n->op && n->op[0]) {
            /* compound assignment: x += y  →  x = (x + y) */
            emit(cg, " = (");
            emit_c_ident(cg, n->sval ? n->sval : "_var");
            emit(cg, " %s ", n->op);
            gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
            emit(cg, ");\n");
        } else {
            emit(cg, " = ");
            gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
            emit(cg, ";\n");
        }
        return;
    }

    /* ── Return ── */
    case NODE_RETURN:
        if (n->sval && !strcmp(n->sval, "__break__")) {
            iemit(cg, "break;\n");
            return;
        }
        if (n->sval && !strcmp(n->sval, "__continue__")) {
            iemit(cg, "continue;\n");
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
        lu_warn(n->line, "Server/ is deprecated — generates stub code, no real network functionality. Will be removed in v4.0.");
        iemit(cg, "/* DEPRECATED: Server/ — stub only */\n");
        iemit(cg, "lu_server_t *_server_q%d = lu_server_new(%d, \"%s\");\n",
              n->block_n, n->block_n, n->sval ? n->sval : "");
        return;

    /* ── Network send (deprecated) ── */
    case NODE_SEND: {
        lu_warn(n->line, "Snd{} is deprecated — generates stub code, no real network functionality. Will be removed in v4.0.");
        if (n->children.count > 0 && n->children.items[0]->kind == NODE_LITERAL_IP) {
            iemit(cg, "/* DEPRECATED */ lu_send(\"%s\", &(", n->sval ? n->sval : "");
            gen_expr(cg, n->children.items[0]);
            emit(cg, "));\n");
        } else {
            iemit(cg, "/* DEPRECATED */ lu_send(\"%s\", NULL);\n", n->sval ? n->sval : "");
        }
        return;
    }

    /* ── Network recv (deprecated) ── */
    case NODE_RECV:
        lu_warn(n->line, "Rec/ is deprecated — generates stub code. Will be removed in v4.0.");
        iemit(cg, "/* DEPRECATED */ lu_recv(");
        gen_expr(cg, n->children.count > 0 ? n->children.items[0] : NULL);
        emit(cg, ");\n");
        return;

    /* ── Bcast (deprecated) ── */
    case NODE_BCAST:
        lu_warn(n->line, "Bcast{} is deprecated — generates stub code. Will be removed in v4.0.");
        iemit(cg, "/* DEPRECATED */ lu_broadcast(\"%s\");\n", n->sval ? n->sval : "");
        return;

    /* ── Route (deprecated) ── */
    case NODE_ROUTE:
        lu_warn(n->line, "Route/ is deprecated — generates stub code. Will be removed in v4.0.");
        iemit(cg, "/* DEPRECATED */ lu_route(");
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
            int fndims = (int)f->ival;
            bool is_array = (f->op && !strcmp(f->op, "array")) || fndims > 0;
            if (is_array && fndims > 0) {
                iemit(cg, "%s %s", lu2c(f->type_name), f->sval ? f->sval : "_f");
                for (int d = 0; d < fndims && d < f->children.count; d++) {
                    emit(cg, "[");
                    gen_expr(cg, f->children.items[d]);
                    emit(cg, "]");
                }
                emit(cg, ";\n");
            } else {
                iemit(cg, "%s %s;\n", lu2c(f->type_name), f->sval ? f->sval : "_f");
            }
        }
        cg->indent--;
        emit(cg, "} %s;\n", n->sval ? n->sval : "_struct");
        return;
    }
    /* ── Union ── */
    case NODE_UNION_DECL: {
        emit(cg, "\ntypedef union {\n");
        cg->indent++;
        for (int i = 0; i < n->children.count; i++) {
            ASTNode *f = n->children.items[i];
            int fndims = (int)f->ival;
            bool is_array = (f->op && !strcmp(f->op, "array")) || fndims > 0;
            if (is_array && fndims > 0) {
                iemit(cg, "%s %s", lu2c(f->type_name), f->sval ? f->sval : "_f");
                for (int d = 0; d < fndims && d < f->children.count; d++) {
                    emit(cg, "[");
                    gen_expr(cg, f->children.items[d]);
                    emit(cg, "]");
                }
                emit(cg, ";\n");
            } else {
                iemit(cg, "%s %s;\n", lu2c(f->type_name), f->sval ? f->sval : "_f");
            }
        }
        cg->indent--;
        emit(cg, "} %s;\n", n->sval ? n->sval : "_union");
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
    case NODE_SPAWN: {
        /* Spawn/ expects a function pointer (void(*)(void)), not an int
           or string. Reject non-identifier arguments with a clear error. */
        ASTNode *arg = n->children.count > 0 ? n->children.items[0] : NULL;
        if (arg && arg->kind != NODE_IDENT && arg->kind != NODE_FUNC_CALL &&
            arg->kind != NODE_FUNC_DECL) {
            lu_warn(n->line,
                "Spawn/ expects a function name, not a %s — "
                "use Spawn/my_function", arg->kind == NODE_LITERAL_INT ? "number" : "value");
            g_parse_error_count++;
            return;
        }
        /* Cast to void(*)(void) to match lu_spawn's signature. This is
           safe because lu_spawn calls fn() with no arguments — the
           callee's extra parameters are simply ignored on x86-64. */
        iemit(cg, "lu_spawn((void(*)(void))(");
        gen_expr(cg, arg);
        emit(cg, "));\n");
        return;
    }

    /* ── Channels ── */
    case NODE_CHAN_DECL:
        cg_sym_add(cg, n->sval ? n->sval : "_ch", "ptr", true);
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
    case NODE_ON: {
        /* On/name callback — callback must be a function name (identifier),
           not a string literal. Reject string literals with a clear error. */
        ASTNode *cb = n->children.count > 0 ? n->children.items[0] : NULL;
        if (cb && cb->kind == NODE_LITERAL_STR) {
            lu_warn(n->line,
                "On/%s: callback must be a function name, not a string — "
                "use On/%s my_handler (without quotes)", n->sval, n->sval);
            g_parse_error_count++;
            return;
        }
        /* Cast to void(*)(void*) to match lu_event_on's signature. The
           callback will be called with a void* argument (the event data).
           If the user's handler has a different parameter type (e.g. str),
           this cast is technically UB but works on x86-64 where all pointer
           types have the same representation. */
        iemit(cg, "lu_event_on(ev_%s, (void(*)(void*))(", n->sval ? n->sval : "ev");
        gen_expr(cg, cb);
        emit(cg, "));\n");
        return;
    }
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

    /* ── defer → cleanup-attribute variable ── */
    case NODE_DEFER: {
        /* defer <stmt> → use GCC nested function + cleanup attribute.
           The nested function can capture variables from the enclosing
           scope, so `defer free(p)` works correctly. */
        if (n->children.count > 0) {
            int did = cg->tmp_counter++;
            char dname[64];
            snprintf(dname, sizeof(dname), "_lu_defer_fn_%d", did);
            /* Emit a nested function (GCC extension). */
            iemit(cg, "void %s(void *_lu_defer_arg) {\n", dname);
            iemit(cg, "  (void)_lu_defer_arg;\n");
            cg->indent++;
            gen_node(cg, n->children.items[0]);
            cg->indent--;
            iemit(cg, "}\n");
            /* Emit the cleanup variable. When this variable goes out of
               scope, the nested function is called. */
            iemit(cg, "__attribute__((cleanup(%s))) int _lu_defer_%d;\n", dname, did);
        }
        return;
    }

    /* ── match/case → lowered to if/else if chain ── */
    case NODE_MATCH: {
        /* children[0] = matched expression, children[1..] = cases.
           Each case is NODE_PROGRAM with annot="case" or "default":
             - "case":   children[0] = value expr, children[1] = body
             - "default": children[0] = body */
        ASTNode *match_expr = n->children.count > 0 ? n->children.items[0] : NULL;
        /* Store the match expression in an int64_t temp so we only
           evaluate it once. Match is intended for integer-like values. */
        int tid = cg->tmp_counter++;
        iemit(cg, "{ int64_t _m%d = (int64_t)(", tid);
        if (match_expr) gen_expr(cg, match_expr);
        else emit(cg, "0");
        emit(cg, ");\n");
        bool first = true;
        for (int i = 1; i < n->children.count; i++) {
            ASTNode *case_node = n->children.items[i];
            if (!case_node->annot) continue;
            if (!strcmp(case_node->annot, "default")) {
                ASTNode *body = case_node->children.count > 0 ? case_node->children.items[0] : NULL;
                if (first) {
                    if (body) gen_block_body(cg, body);
                } else {
                    iemit(cg, "else {\n");
                    cg->indent++;
                    if (body) gen_block_body(cg, body);
                    cg->indent--;
                    iemit(cg, "}\n");
                }
                first = false;
            } else if (!strcmp(case_node->annot, "case")) {
                ASTNode *case_val = case_node->children.count > 0 ? case_node->children.items[0] : NULL;
                ASTNode *body = case_node->children.count > 1 ? case_node->children.items[1] : NULL;
                if (first) {
                    iemit(cg, "if (_m%d == (int64_t)(", tid);
                    if (case_val) gen_expr(cg, case_val);
                    else emit(cg, "0");
                    emit(cg, ")) {\n");
                    first = false;
                } else {
                    iemit(cg, "else if (_m%d == (int64_t)(", tid);
                    if (case_val) gen_expr(cg, case_val);
                    else emit(cg, "0");
                    emit(cg, ")) {\n");
                }
                cg->indent++;
                if (body) gen_block_body(cg, body);
                cg->indent--;
                iemit(cg, "}\n");
            }
        }
        iemit(cg, "}\n");
        return;
    }

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
"/* Suppress unused-function warnings — the runtime includes many helpers\n"
"   that may not be used by every program. */\n"
"#pragma GCC diagnostic push\n"
"#pragma GCC diagnostic ignored \"-Wunused-function\"\n"
"#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
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
"/* String concatenation: returns a freshly malloc'd string. */\n"
"static char *lu_str_concat(const char *a, const char *b) {\n"
"    size_t la = a ? strlen(a) : 0;\n"
"    size_t lb = b ? strlen(b) : 0;\n"
"    char *out = (char*)malloc(la + lb + 1);\n"
"    if (!out) { fprintf(stderr, \"[Lu MEM] out of memory\\n\"); exit(ERR_MEM); }\n"
"    if (la) memcpy(out, a, la);\n"
"    if (lb) memcpy(out + la, b, lb);\n"
"    out[la + lb] = '\\0';\n"
"    return out;\n"
"}\n"
"/* Convert non-string values to string for use in concatenation. */\n"
"static char *lu_str_from_int(int64_t v) {\n"
"    char buf[32];\n"
"    snprintf(buf, sizeof(buf), \"%lld\", (long long)v);\n"
"    return lu_str_concat(buf, \"\");\n"
"}\n"
"static char *lu_str_from_float(double v) {\n"
"    char buf[64];\n"
"    snprintf(buf, sizeof(buf), \"%g\", v);\n"
"    return lu_str_concat(buf, \"\");\n"
"}\n"
"static char *lu_str_from_bool(bool v) {\n"
"    return lu_str_concat(v ? \"true\" : \"false\", \"\");\n"
"}\n"
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
"/* ── Standard library: math ── */\n"
"#include <math.h>\n"
"/* Math functions (sqrt, sin, cos, tan, floor, ceil, round, pow) come from math.h.\n"
"   User code calls them directly: sqrt(x), sin(x), etc. */\n"
"static int64_t lu_min(int64_t a, int64_t b) { return a < b ? a : b; }\n"
"static int64_t lu_max(int64_t a, int64_t b) { return a > b ? a : b; }\n"
"static int64_t lu_abs_int(int64_t v) { return v < 0 ? -v : v; }\n"
"static double lu_abs_float(double v) { return v < 0 ? -v : v; }\n"
"\n"
"/* ── Standard library: string ── */\n"
"static int lu_str_len(const char *s) { return s ? (int)strlen(s) : 0; }\n"
"static char *lu_str_upper(const char *s) {\n"
"    if (!s) return lu_str_concat(\"\", \"\");\n"
"    char *out = (char*)malloc(strlen(s) + 1);\n"
"    if (!out) { exit(ERR_MEM); }\n"
"    int i;\n"
"    for (i = 0; s[i]; i++) out[i] = toupper((unsigned char)s[i]);\n"
"    out[i] = '\\0';\n"
"    return out;\n"
"}\n"
"static char *lu_str_lower(const char *s) {\n"
"    if (!s) return lu_str_concat(\"\", \"\");\n"
"    char *out = (char*)malloc(strlen(s) + 1);\n"
"    if (!out) { exit(ERR_MEM); }\n"
"    int i;\n"
"    for (i = 0; s[i]; i++) out[i] = tolower((unsigned char)s[i]);\n"
"    out[i] = '\\0';\n"
"    return out;\n"
"}\n"
"static bool lu_str_contains(const char *hay, const char *needle) {\n"
"    if (!hay || !needle) return false;\n"
"    return strstr(hay, needle) != NULL;\n"
"}\n"
"static char *lu_str_replace(const char *s, const char *from, const char *to) {\n"
"    if (!s || !from || !to || !*from) return lu_str_concat(s ? s : \"\", \"\");\n"
"    size_t slen = strlen(s), flen = strlen(from), tlen = strlen(to);\n"
"    char *out = (char*)malloc(slen * (tlen > flen ? tlen : flen) + tlen + 1);\n"
"    if (!out) { exit(ERR_MEM); }\n"
"    size_t oi = 0;\n"
"    const char *p = s;\n"
"    while (*p) {\n"
"        if (strncmp(p, from, flen) == 0) {\n"
"            memcpy(out + oi, to, tlen); oi += tlen; p += flen;\n"
"        } else {\n"
"            out[oi++] = *p++;\n"
"        }\n"
"    }\n"
"    out[oi] = '\\0';\n"
"    return out;\n"
"}\n"
"\n"
"/* ── Standard library: range() ── */\n"
"/* range(n) returns a pointer to a static array [0, 1, ..., n-1].\n"
"   For `for x in range(n)` to work, we need a persistent array. */\n"
"static int *lu_range(int n) {\n"
"    if (n < 0) n = 0;\n"
"    int *arr = (int*)malloc(sizeof(int) * (n > 0 ? n : 1));\n"
"    if (!arr) { exit(ERR_MEM); }\n"
"    for (int i = 0; i < n; i++) arr[i] = i;\n"
"    return arr;\n"
"}\n"
"/* range_len(n) returns the length of a range — used by for-each codegen. */\n"
"static int lu_range_len(int n) { return n < 0 ? 0 : n; }\n"
"\n"
"/* ── Standard library: io ── */\n"
"static char *lu_read_file(const char *path) {\n"
"    FILE *f = fopen(path, \"rb\");\n"
"    if (!f) return lu_str_concat(\"\", \"\");\n"
"    fseek(f, 0, SEEK_END);\n"
"    long sz = ftell(f);\n"
"    rewind(f);\n"
"    char *buf = (char*)malloc(sz + 1);\n"
"    if (!buf) { fclose(f); exit(ERR_MEM); }\n"
"    size_t rd = fread(buf, 1, sz, f);\n"
"    buf[rd] = '\\0';  /* use actual bytes read, not requested */\n"
"    fclose(f);\n"
"    return buf;\n"
"}\n"
"static void lu_write_file(const char *path, const char *content) {\n"
"    FILE *f = fopen(path, \"wb\");\n"
"    if (!f) return;\n"
"    fputs(content ? content : \"\", f);\n"
"    fclose(f);\n"
"}\n"
"static char *lu_input(const char *prompt) {\n"
"    if (prompt) { printf(\"%s\", prompt); fflush(stdout); }\n"
"    char buf[4096];\n"
"    if (!fgets(buf, sizeof(buf), stdin)) return lu_str_concat(\"\", \"\");\n"
"    /* strip trailing newline */\n"
"    size_t n = strlen(buf);\n"
"    if (n > 0 && buf[n-1] == '\\n') buf[n-1] = '\\0';\n"
"    return lu_str_concat(buf, \"\");\n"
"}\n"
"#define read_file(p) lu_read_file(p)\n"
"#define write_file(p,c) lu_write_file(p,c)\n"
"#define input(p) lu_input(p)\n"
"\n"
"/* ── Vector<T> built-in (int version) ── */\n"
"typedef struct { int *data; int len; int cap; } lu_vector_int;\n"
"static void lu_vector_int_init(lu_vector_int *v) { v->data = NULL; v->len = 0; v->cap = 0; }\n"
"static void lu_vector_int_push(lu_vector_int *v, int val) {\n"
"    if (v->len >= v->cap) {\n"
"        v->cap = v->cap ? v->cap * 2 : 8;\n"
"        v->data = (int*)realloc(v->data, sizeof(int) * v->cap);\n"
"        if (!v->data) { exit(ERR_MEM); }\n"
"    }\n"
"    v->data[v->len++] = val;\n"
"}\n"
"static int lu_vector_int_get(lu_vector_int *v, int i) {\n"
"    if (i < 0 || i >= v->len) return 0;\n"
"    return v->data[i];\n"
"}\n"
"static void lu_vector_int_set(lu_vector_int *v, int i, int val) {\n"
"    if (i >= 0 && i < v->len) v->data[i] = val;\n"
"}\n"
"static int lu_vector_int_len(lu_vector_int *v) { return v->len; }\n"
"static int lu_vector_int_pop(lu_vector_int *v) {\n"
"    if (v->len == 0) return 0;\n"
"    return v->data[--v->len];\n"
"}\n"
"static void lu_vector_int_free(lu_vector_int *v) {\n"
"    if (v->data) { free(v->data); v->data = NULL; v->len = 0; v->cap = 0; }\n"
"}\n"
"\n"
"/* ── Smart pointers: Unique<T> and Shared<T> ── */\n"
"/* Unique<T> is a pointer that is automatically freed when it goes out of scope.\n"
"   Usage: Unique<int> p = new int(42)\n"
"   The cleanup attribute ensures free() is called at scope exit. */\n"
"static void lu_cleanup_free(void *p) {\n"
"    void **pp = (void**)p;\n"
"    if (*pp) { free(*pp); *pp = NULL; }\n"
"}\n"
"/* defer support: a cleanup handler that runs a stored function. */\n"
"typedef struct { void (*fn)(void*); void *data; } _lu_defer_t;\n"
"static void _lu_defer_run(_lu_defer_t *d) {\n"
"    if (d && d->fn) d->fn(d->data);\n"
"}\n"
"/* Helper to wrap free() for defer. */\n"
"static void _lu_defer_free(void *p) { if (p) free(p); }\n"
"/* Slice type: { void* ptr; int len; } — used for arr[start:end].\n"
"   The ptr is void* so the same slice type works for any element type. */\n"
"typedef struct { void* ptr; int len; } lu_slice_t;\n"
"/* Shared<T> is a reference-counted pointer. */\n"
"typedef struct { void *ptr; int *refcount; } lu_shared_ptr;\n"
"static lu_shared_ptr lu_shared_new(void *p) {\n"
"    lu_shared_ptr sp;\n"
"    sp.ptr = p;\n"
"    sp.refcount = (int*)malloc(sizeof(int));\n"
"    if (sp.refcount) *sp.refcount = 1;\n"
"    return sp;\n"
"}\n"
"static lu_shared_ptr lu_shared_copy(lu_shared_ptr sp) {\n"
"    if (sp.refcount) (*sp.refcount)++;\n"
"    return sp;\n"
"}\n"
"static void lu_shared_cleanup(void *p) {\n"
"    lu_shared_ptr *sp = (lu_shared_ptr*)p;\n"
"    if (sp->refcount) {\n"
"        (*sp->refcount)--;\n"
"        if (*sp->refcount <= 0) {\n"
"            if (sp->ptr) free(sp->ptr);\n"
"            free(sp->refcount);\n"
"            sp->ptr = NULL;\n"
"            sp->refcount = NULL;\n"
"        }\n"
"    }\n"
"}\n"
"#define Unique(T) __attribute__((cleanup(lu_cleanup_free))) T*\n"
"#define Shared(T) __attribute__((cleanup(lu_shared_cleanup))) lu_shared_ptr\n"
"#define shared_new(T, val) lu_shared_new(({ T *_t = (T*)malloc(sizeof(T)); *_t = (val); _t; }))\n"
"#define shared_get(sp) ((sp).ptr)\n"
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
"/* End of runtime — restore diagnostic state. */\n"
"#pragma GCC diagnostic pop\n"
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
   Collect function/method return types so `auto x = f()`
   can infer the correct C type during codegen.
   The codegen symbol table (cg->syms) normally only holds
   variables — this pre-pass adds function entries so that
   infer_c_type() can look up return types.
   ───────────────────────────────────────────── */
static void collect_functions(Codegen *cg, ASTNode *node) {
    if (!node) return;
    if ((node->kind == NODE_FUNC_DECL || node->kind == NODE_ASYNC_FUNC) && node->sval) {
        cg_sym_add(cg, node->sval, node->type_name ? node->type_name : "void", false);
    }
    /* Class methods: register as Type_method, AND register the class
       itself in the class registry for inheritance resolution. */
    if (node_is_class_like(node) && node->sval) {
        const char *cn = node->sval;
        cg_class_register(cg, cn, node);
        for (int i = 0; i < node->children.count; i++) {
            ASTNode *m = node->children.items[i];
            if (m->kind == NODE_METHOD_DECL && m->sval) {
                char fname[256];
                snprintf(fname, sizeof(fname), "%s_%s", cn, m->sval);
                cg_sym_add(cg, fname, m->type_name ? m->type_name : "void", false);
            }
        }
    }
    for (int i = 0; i < node->children.count; i++)
        collect_functions(cg, node->children.items[i]);
}

/* Pre-pass: emit all lambda expressions as static functions before main.
   This walks the AST, finds every NODE_FUNC_DECL with annot="lambda",
   assigns it a unique name, and emits the function at file scope.
   The lambda's op field is rewritten to "_lu_lambda_<id>" so gen_expr
   can just emit "&_lu_lambda_<id>" when it encounters the lambda. */
static int g_lambda_counter = 0;
static void emit_lambdas(Codegen *cg, ASTNode *node) {
    if (!node) return;
    if (node->kind == NODE_FUNC_DECL && node->annot && !strcmp(node->annot, "lambda")) {
        int lid = g_lambda_counter++;
        char lname[64];
        snprintf(lname, sizeof(lname), "_lu_lambda_%d", lid);
        /* Store the name in op (replacing the signature). */
        free(node->op);
        node->op = lu_strdup(lname);
        node->ival = lid;  /* also store id for gen_expr */
        /* Parse the signature from type_name. */
        const char *sig = node->type_name ? node->type_name : "fn:()->void";
        const char *params_start = sig + 3;
        const char *arrow = strstr(params_start, "->");
        char ret_type[64] = "void";
        if (arrow) {
            size_t rlen = strlen(arrow + 2);
            if (rlen >= sizeof(ret_type)) rlen = sizeof(ret_type) - 1;
            memcpy(ret_type, arrow + 2, rlen);
            ret_type[rlen] = '\0';
        }
        /* plen was used for anonymous-struct slice codegen but is not
           needed here since we emit params from the lambda's children. */
        (void)params_start;
        emit(cg, "\nstatic ");
        emit_c_decl(cg, ret_type, lname);
        emit(cg, "(");
        /* Emit params: use the names from the lambda's NODE_VAR_DECL
           children (collected during parsing). The body is the last child. */
        int n_params = 0;
        for (int i = 0; i < node->children.count; i++) {
            ASTNode *c = node->children.items[i];
            if (c->kind == NODE_VAR_DECL) n_params++;
            else break;
        }
        for (int i = 0; i < n_params; i++) {
            ASTNode *c = node->children.items[i];
            if (i > 0) emit(cg, ", ");
            emit_c_decl(cg, c->type_name, c->sval);
        }
        emit(cg, ") {\n");
        /* Save and set current_class to NULL (lambdas have no `this`). */
        ASTNode *saved = cg->current_class;
        cg->current_class = NULL;
        /* Register params in the codegen symbol table. */
        for (int i = 0; i < n_params; i++) {
            ASTNode *c = node->children.items[i];
            if (c->sval) cg_sym_add(cg, c->sval, c->type_name, false);
        }
        cg->indent++;
        /* The body is the last child. */
        if (node->children.count > n_params)
            gen_block_body(cg, node->children.items[node->children.count - 1]);
        cg->indent--;
        cg->current_class = saved;
        emit(cg, "}\n");
    }
    for (int i = 0; i < node->children.count; i++)
        emit_lambdas(cg, node->children.items[i]);
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

    /* ── Pre-pass: collect function/method return types (for `auto x = f()`) ── */
    collect_functions(&cg, root);

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

    /* Pre-pass: emit all lambda functions at file scope (before main
       and before the blocks that use them). */
    g_lambda_counter = 0;
    emit_lambdas(&cg, root);

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