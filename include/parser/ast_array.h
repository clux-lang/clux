#ifndef _H_CLUX_PARSER_AST_ARRAY_
#define _H_CLUX_PARSER_AST_ARRAY_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * 数组类型表达式节点：[N]T
 *   - base_type：基础类型表达式（AST_IDENT("i32") / AST_CONST(...) / 嵌套 AST_ARRAY）
 *   - length：长度表达式（AST_INT_LIT / AST_IDENT / 编译期计算）
 * 数组类型本身是表达式节点（如 sizeof([3]i32) 天然成立），纳入表达式体系。
 * 由 parse_primary 的 '[' 分支构造。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *base_type;  /* 基础类型（如 AST_IDENT / 嵌套 AST_ARRAY） */
    ast_node_t *length;     /* 长度表达式（如 AST_INT_LIT / AST_IDENT） */
} ast_array_t;

static inline ast_node_t *ast_array_new(arena_t *arena,
                                        uint32_t tok_begin, uint32_t tok_end) {
    ast_array_t *n = (ast_array_t *)arena_calloc(
        arena, 1, sizeof(ast_array_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_ARRAY;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_ARRAY_ */
