#include "lu.h"
#include <stdarg.h>

char *lu_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (!d) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(d, s, len);
    return d;
}

void node_list_add(NodeList *l, ASTNode *n) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, sizeof(ASTNode*) * l->cap);
        if (!l->items) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    l->items[l->count++] = n;
}

ASTNode *node_new(NodeKind kind, int line) {
    ASTNode *n = calloc(1, sizeof(ASTNode));
    if (!n) { fprintf(stderr, "out of memory\n"); exit(1); }
    n->kind = kind;
    n->line = line;
    return n;
}

void lu_error(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "\033[31m[LU ERROR]\033[0m line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void lu_warn(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "\033[33m[LU WARN]\033[0m line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void ast_free(ASTNode *node) {
    if (!node) return;
    for (int i = 0; i < node->children.count; i++)
        ast_free(node->children.items[i]);
    free(node->children.items);
    free(node->sval);
    free(node->type_name);
    free(node->op);
    free(node->annot);
    free(node);
}

static const char *node_kind_name(NodeKind k) {
    switch (k) {
    case NODE_PROGRAM:      return "PROGRAM";
    case NODE_LANG_DECL:    return "LANG_DECL";
    case NODE_IMPORT:       return "IMPORT";
    case NODE_MODE:         return "MODE";
    case NODE_DEF_CONST:    return "DEF_CONST";
    case NODE_DEF_CONFIG:   return "DEF_CONFIG";
    case NODE_DEFINE:       return "DEFINE";
    case NODE_INCLUDE:      return "INCLUDE";
    case NODE_BLOCK:        return "BLOCK";
    case NODE_IF:           return "IF";
    case NODE_LOOP:         return "LOOP";
    case NODE_LOOP_WHILE:   return "LOOP_WHILE";
    case NODE_LOOP_EACH:    return "LOOP_EACH";
    case NODE_PR:           return "PR";
    case NODE_TO:           return "TO";
    case NODE_SET:          return "SET";
    case NODE_VAR_DECL:     return "VAR_DECL";
    case NODE_FUNC_DECL:    return "FUNC_DECL";
    case NODE_ASYNC_FUNC:   return "ASYNC_FUNC";
    case NODE_FUNC_CALL:    return "FUNC_CALL";
    case NODE_RETURN:       return "RETURN";
    case NODE_LINREF:       return "LINREF";
    case NODE_USER_DECL:    return "USER_DECL";
    case NODE_SERVER_DECL:  return "SERVER_DECL";
    case NODE_SEND:         return "SEND";
    case NODE_RECV:         return "RECV";
    case NODE_BCAST:        return "BCAST";
    case NODE_ROUTE:        return "ROUTE";
    case NODE_INQ:          return "INQ";
    case NODE_PTR_DECL:     return "PTR_DECL";
    case NODE_ALLOC:        return "ALLOC";
    case NODE_FREE:         return "FREE";
    case NODE_MEMSET:       return "MEMSET";
    case NODE_MEMCPY:       return "MEMCPY";
    case NODE_TRY:          return "TRY";
    case NODE_THROW:        return "THROW";
    case NODE_STRUCT_DECL:  return "STRUCT";
    case NODE_ENUM_DECL:    return "ENUM";
    case NODE_NAMESPACE:    return "NAMESPACE";
    case NODE_MODULE:       return "MODULE";
    case NODE_AWAIT:        return "AWAIT";
    case NODE_SPAWN:        return "SPAWN";
    case NODE_CHAN_DECL:     return "CHAN_DECL";
    case NODE_CHAN_SEND:     return "CHAN_SEND";
    case NODE_CHAN_RECV:     return "CHAN_RECV";
    case NODE_SELECT:        return "SELECT";
    case NODE_EVENT:         return "EVENT";
    case NODE_ON:            return "ON";
    case NODE_EMIT:          return "EMIT";
    case NODE_LOG:           return "LOG";
    case NODE_ASSERT:        return "ASSERT";
    case NODE_ANNOT:         return "ANNOT";
    case NODE_OPT:           return "OPT";
    case NODE_EXPR_BINOP:    return "BINOP";
    case NODE_EXPR_UNOP:     return "UNOP";
    case NODE_EXPR_TERNARY:  return "TERNARY";
    case NODE_EXPR_INDEX:    return "INDEX";
    case NODE_EXPR_FIELD:    return "FIELD";
    case NODE_EXPR_DEREF:    return "DEREF";
    case NODE_EXPR_REF:      return "REF";
    case NODE_LITERAL_INT:   return "LIT_INT";
    case NODE_LITERAL_FLOAT: return "LIT_FLOAT";
    case NODE_LITERAL_STR:   return "LIT_STR";
    case NODE_LITERAL_BOOL:  return "LIT_BOOL";
    case NODE_LITERAL_IP:    return "LIT_IP";
    case NODE_LITERAL_COR:   return "LIT_COR";
    case NODE_IDENT:         return "IDENT";
    case NODE_CLASS_DECL:    return "CLASS";
    case NODE_INTERFACE_DECL:return "INTERFACE";
    case NODE_IMPL_DECL:     return "IMPL";
    case NODE_TEMPLATE_DECL: return "TEMPLATE";
    case NODE_METHOD_DECL:   return "METHOD";
    case NODE_CONSTRUCTOR:   return "CONSTRUCTOR";
    case NODE_DESTRUCTOR:    return "DESTRUCTOR";
    case NODE_NEW_EXPR:      return "NEW";
    case NODE_DELETE_EXPR:   return "DELETE";
    case NODE_OBJ_DECL:      return "OBJ_DECL";
    case NODE_CL_CALL:       return "CL_CALL";
    case NODE_DAM_CALL:      return "DAM_CALL";
    case NODE_ONL_CALL:      return "ONL_CALL";
    case NODE_SND_GOTO:      return "SND_GOTO";
    default:                 return "?";
    }
}

void ast_print(ASTNode *node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth * 2; i++) printf(i % 2 == 0 ? "|" : " ");
    printf("%s", node_kind_name(node->kind));
    if (node->sval)      printf("  sval='%s'", node->sval);
    if (node->type_name) printf("  type='%s'", node->type_name);
    if (node->op)        printf("  op='%s'", node->op);
    if (node->block_n)   printf("  q=%d", node->block_n);
    if (node->kind == NODE_LITERAL_INT)   printf("  val=%lld", (long long)node->ival);
    if (node->kind == NODE_LITERAL_FLOAT) printf("  val=%g", node->fval);
    if (node->kind == NODE_LITERAL_BOOL)  printf("  val=%s", node->bval?"true":"false");
    printf("  (line %d)\n", node->line);
    for (int i = 0; i < node->children.count; i++)
        ast_print(node->children.items[i], depth + 1);
}

/* ─────────────────────────────────────────────
   Block Registry helpers
   ───────────────────────────────────────────── */
BlockInfo *breg_find(BlockRegistry *br, int block_n) {
    for (int i = 0; i < br->count; i++)
        if (br->entries[i].block_n == block_n) return &br->entries[i];
    return NULL;
}

BlockInfo *breg_get_or_create(BlockRegistry *br, int block_n) {
    BlockInfo *bi = breg_find(br, block_n);
    if (bi) return bi;
    if (br->count >= BLOCK_REG_MAX) return NULL;
    bi = &br->entries[br->count++];
    memset(bi, 0, sizeof(*bi));
    bi->block_n = block_n;
    return bi;
}