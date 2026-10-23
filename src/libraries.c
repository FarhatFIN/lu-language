#include "lu.h"

static const LibraryFunction tensor_functions[] = {
    {"tensor_zeros", "Tensor", 2, {"int", "int"}},
    {"tensor_ones", "Tensor", 2, {"int", "int"}},
    {"tensor_full", "Tensor", 3, {"int", "int", "float"}},
    {"tensor_eye", "Tensor", 1, {"int"}},
    {"tensor_copy", "Tensor", 1, {"Tensor"}},
    {"tensor_add", "Tensor", 2, {"Tensor", "Tensor"}},
    {"tensor_sub", "Tensor", 2, {"Tensor", "Tensor"}},
    {"tensor_mul", "Tensor", 2, {"Tensor", "Tensor"}},
    {"tensor_div", "Tensor", 2, {"Tensor", "Tensor"}},
    {"tensor_scale", "Tensor", 2, {"Tensor", "float"}},
    {"tensor_matmul", "Tensor", 2, {"Tensor", "Tensor"}},
    {"tensor_matvec", "Tensor", 2, {"Tensor", "Tensor"}},
    {"tensor_transpose", "Tensor", 1, {"Tensor"}},
    {"tensor_abs", "Tensor", 1, {"Tensor"}},
    {"tensor_relu", "Tensor", 1, {"Tensor"}},
    {"tensor_sigmoid", "Tensor", 1, {"Tensor"}},
    {"tensor_tanh", "Tensor", 1, {"Tensor"}},
    {"tensor_clip", "Tensor", 3, {"Tensor", "float", "float"}},
    {"tensor_get", "float", 3, {"Tensor", "int", "int"}},
    {"tensor_set", "void", 4, {"Tensor", "int", "int", "float"}},
    {"tensor_fill", "void", 2, {"Tensor", "float"}},
    {"tensor_dot", "float", 2, {"Tensor", "Tensor"}},
    {"tensor_sum", "float", 1, {"Tensor"}},
    {"tensor_max", "float", 1, {"Tensor"}},
    {"tensor_min", "float", 1, {"Tensor"}},
    {"tensor_argmax", "int", 1, {"Tensor"}},
    {"tensor_mean", "float", 1, {"Tensor"}},
    {"tensor_rows", "int", 1, {"Tensor"}},
    {"tensor_cols", "int", 1, {"Tensor"}},
    {"tensor_print", "void", 1, {"Tensor"}},
    {"tensor_free", "void", 1, {"Tensor"}}
};

const LibraryFunction *tensor_api(size_t *count) {
    *count = sizeof(tensor_functions) / sizeof(tensor_functions[0]);
    return tensor_functions;
}

const LibraryFunction *tensor_function(const char *name) {
    size_t count;
    const LibraryFunction *api = tensor_api(&count);
    for (size_t i = 0; name && i < count; i++)
        if (!strcmp(name, api[i].name)) return &api[i];
    return NULL;
}

bool is_tensor_import(const ASTNode *node) {
    return node && node->kind == NODE_INCLUDE && node->sval &&
           !strcmp(node->sval, "<tensor.h>");
}

bool has_tensor_import(const ASTNode *root) {
    for (int i = 0; root && i < root->children.count; i++)
        if (is_tensor_import(root->children.items[i])) return true;
    return false;
}

/* Match a type token, including inside Vector<Tensor>, ?Tensor and fn types. */
static bool contains_tensor_type(const char *text) {
    for (const char *p = text; p && *p; p++) {
        if ((p == text || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
            !strncmp(p, "Tensor", 6) &&
            !isalnum((unsigned char)p[6]) && p[6] != '_') return true;
    }
    return false;
}

static void check_library_node(ASTNode *node, bool enabled, int depth) {
    if (!node) return;
    if ((node->kind == NODE_IMPORT || node->kind == NODE_INCLUDE) && node->sval &&
        (!strcmp(node->sval, "<tensor.h>") || !strcmp(node->sval, "\"tensor.h\"")) &&
        !is_tensor_import(node))
        lu_error(node->line, "tensor library must be enabled exactly with #include <tensor.h>");
    if (is_tensor_import(node) && depth != 1)
        lu_error(node->line, "tensor.h must be included at file scope");

    bool type_use = contains_tensor_type(node->type_name) ||
        (node->kind == NODE_NEW_EXPR && contains_tensor_type(node->sval)) ||
        (node->kind == NODE_EXPR_UNOP && node->op &&
         !strncmp(node->op, "(cast:", 6) && contains_tensor_type(node->op));
    bool api_use = (node->kind == NODE_FUNC_CALL || node->kind == NODE_IDENT) &&
                   tensor_function(node->sval);
    if (!enabled && (type_use || api_use))
        lu_error(node->line, "tensor support requires #include <tensor.h>");

    /* Runtime internals are not an alternate way to enable the library. */
    if ((node->kind == NODE_FUNC_CALL || node->kind == NODE_IDENT) &&
        node->sval && !strncmp(node->sval, "lu_tensor_", 10))
        lu_error(node->line, "private tensor API '%s'; use tensor.h public functions", node->sval);

    if (node->kind == NODE_FUNC_DECL && tensor_function(node->sval))
        lu_error(node->line, "'%s' is reserved by tensor.h", node->sval);
    if ((node->kind == NODE_CLASS_DECL || node->kind == NODE_STRUCT_DECL ||
         node->kind == NODE_ENUM_DECL || node->kind == NODE_UNION_DECL) &&
        node->sval && !strcmp(node->sval, "Tensor"))
        lu_error(node->line, "type 'Tensor' is reserved by tensor.h");

    if (enabled && node->kind == NODE_FUNC_CALL) {
        const LibraryFunction *fn = tensor_function(node->sval);
        if (fn && node->children.count != fn->arity)
            lu_error(node->line, "'%s' expects %d argument(s), got %d",
                     fn->name, fn->arity, node->children.count);
    }
    for (int i = 0; i < node->children.count; i++)
        check_library_node(node->children.items[i], enabled, depth + 1);
}

void check_library_usage(ASTNode *root) {
    check_library_node(root, has_tensor_import(root), 0);
}
