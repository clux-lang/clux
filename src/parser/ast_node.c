#include "parser/ast_node.h"

/* ---- Kind name table ---- */

static const char *g_kind_names[] = {
    [AST_PROGRAM]    = "program",
    [AST_FUNC_DEF]   = "func_def",
    [AST_VAR_DEF]    = "var_def",
    [AST_ASSIGN]     = "assign",
    [AST_IF]         = "if",
    [AST_WHILE]      = "while",
    [AST_FOR]        = "for",
    [AST_RETURN]     = "return",
    [AST_BREAK]      = "break",
    [AST_CONTINUE]   = "continue",
    [AST_BLOCK]      = "block",
    [AST_EXPR_STMT]  = "expr_stmt",
    [AST_EMPTY_STMT] = "empty_stmt",
    [AST_BINARY]     = "binary",
    [AST_UNARY]      = "unary",
    [AST_CALL]       = "call",
    [AST_MEMBER]     = "member",
    [AST_INDEX]      = "index",
    [AST_ARRAY]      = "array",
    [AST_CONSTRUCT]  = "construct",
    [AST_INT_LIT]    = "int_lit",
    [AST_FLOAT_LIT]  = "float_lit",
    [AST_BOOL_LIT]   = "bool_lit",
    [AST_STRING_LIT] = "string_lit",
    [AST_CHAR_LIT]   = "char_lit",
    [AST_IDENT]      = "ident",
    [AST_UNDEF]      = "undef",
    [AST_CONST]      = "const",
    [AST_VOLATILE]   = "volatile",
    [AST_FUNC_TYPE]  = "func_type",
    [AST_TYPE_REF]   = "type_ref",
    [AST_FUNC_REF]   = "func_ref",
    [AST_TERNARY]    = "ternary",
    [AST_ERROR]      = "error",
};

/* ---- Public API ---- */

void ast_append(ast_node_t **head, ast_node_t **last,
                ast_node_t *parent, ast_node_t *node) {
    if (!head || !last || !node) return;

    node->parent = parent;
    node->next   = NULL;

    if (!*head) {
        *head = node;
        *last = node;
    } else {
        (*last)->next = node;
        *last = node;
    }
}

const char *ast_kind_name(ast_kind_t kind) {
    if (kind < 0 || kind >= AST_KIND_COUNT) return "unknown";
    return g_kind_names[kind];
}
