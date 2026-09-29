#ifndef _H_CLUX_PARSER_AST_ALIGNOF_
#define _H_CLUX_PARSER_AST_ALIGNOF_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/* alignof(T) / alignof(expr)：类型或表达式 → u64 编译期常量（m2-design §8）。
 * 与 sizeof 同构：操作数只做 shadow 求值，sema 求值后折叠为 AST_INT_LIT(u64)。 */
typedef struct {
    ast_node_t   base;
    ast_node_t  *operand;  /* 类型表达式或值表达式 */
} ast_alignof_t;

static inline ast_node_t *ast_alignof_new(arena_t *arena,
                                          uint32_t tok_begin, uint32_t tok_end) {
    ast_alignof_t *n = (ast_alignof_t *)arena_calloc(
        arena, 1, sizeof(ast_alignof_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_ALIGNOF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif
