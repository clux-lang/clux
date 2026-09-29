#ifndef _H_CLUX_PARSER_AST_SIZEOF_
#define _H_CLUX_PARSER_AST_SIZEOF_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/* sizeof(T) / sizeof(expr)：类型或表达式 → u64 编译期常量（m2-design §8）。
 * 操作数只做 shadow 求值（仅取类型），不真实执行——运算符自身是
 * SEMA→CTFE 桥梁，sema 求值后折叠为 AST_INT_LIT(u64)。 */
typedef struct {
    ast_node_t   base;
    ast_node_t  *operand;  /* 类型表达式或值表达式 */
} ast_sizeof_t;

static inline ast_node_t *ast_sizeof_new(arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end) {
    ast_sizeof_t *n = (ast_sizeof_t *)arena_calloc(
        arena, 1, sizeof(ast_sizeof_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_SIZEOF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif
