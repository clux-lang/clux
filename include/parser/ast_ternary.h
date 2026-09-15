#ifndef _H_CLUX_PARSER_AST_TERNARY_
#define _H_CLUX_PARSER_AST_TERNARY_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * 三元条件表达式节点：cond ? then_branch : else_branch
 *   - cond：条件表达式（sema 校验 bool）
 *   - then_branch：条件为真时求值的分支
 *   - else_branch：条件为假时求值的分支
 * 惰性求值（只求值选中分支）在 ctfe/sema/compiler 三层分别实现。
 * 由 parse_expr_prec 的 '?' 分支构造（右结合，优先级与赋值同级）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *cond;
    ast_node_t *then_branch;
    ast_node_t *else_branch;
} ast_ternary_t;

static inline ast_node_t *ast_ternary_new(arena_t *arena,
                                          uint32_t tok_begin, uint32_t tok_end) {
    ast_ternary_t *n = (ast_ternary_t *)arena_calloc(
        arena, 1, sizeof(ast_ternary_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_TERNARY;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_TERNARY_ */
