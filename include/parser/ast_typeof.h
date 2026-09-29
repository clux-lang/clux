#ifndef _H_CLUX_PARSER_AST_TYPEOF_
#define _H_CLUX_PARSER_AST_TYPEOF_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/* typeof(expr)：表达式 → type value（编译期实体，m2-design §8）。
 * 操作数只做 shadow 求值，sema 求值后折叠为 AST_TYPE_REF（compiler
 * 发 LOAD_TYPE <id> 加载真实类型值，零感知）。 */
typedef struct {
    ast_node_t   base;
    ast_node_t  *operand;  /* 值表达式 */
} ast_typeof_t;

static inline ast_node_t *ast_typeof_new(arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end) {
    ast_typeof_t *n = (ast_typeof_t *)arena_calloc(
        arena, 1, sizeof(ast_typeof_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_TYPEOF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif
