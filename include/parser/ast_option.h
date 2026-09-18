#ifndef _H_CLUX_PARSER_AST_OPTION_
#define _H_CLUX_PARSER_AST_OPTION_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/* ?T optional 类型修饰（类型即表达式）。
 * '?' 前缀与 const/volatile 同族：真实类型 kind（option_type_t），
 * 嵌套节点表达递归修饰（?[N]T / ?const i32 等，消费层按嵌套顺序收敛）。
 * 注意：普通表达式位置 '?' 是三元条件运算符（parse_expr_prec 2a）；
 * 仅原子位置（parse_unary 前缀）的 '?' 解析为 optional 修饰，二者天然区分。 */
typedef struct {
    ast_node_t   base;
    ast_node_t  *sub;      /* 被包裹的类型表达式 T */
} ast_option_t;

static inline ast_node_t *ast_option_new(arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end) {
    ast_option_t *n = (ast_option_t *)arena_calloc(
        arena, 1, sizeof(ast_option_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_OPTION;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_OPTION_ */
