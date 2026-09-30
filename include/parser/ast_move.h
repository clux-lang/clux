#ifndef _H_CLUX_PARSER_AST_MOVE_
#define _H_CLUX_PARSER_AST_MOVE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/* 所有权原语（m3-design §7）：move(x) / clone(x)。
 * op 为原语关键字 token（"move" / "clone"）；operand 为被转移/拷贝的
 * 表达式。move 返回 fatal（原对象进 TDZ），clone 返回 fatal（深拷贝）。 */
typedef struct {
    ast_node_t  base;
    token_t    *op;         /* 原语关键字 token（move/clone） */
    ast_node_t *operand;    /* 操作数表达式 */
} ast_move_t;

static inline ast_node_t *ast_move_new(arena_t *arena,
                                       uint32_t tok_begin, uint32_t tok_end) {
    ast_move_t *n = (ast_move_t *)arena_calloc(
        arena, 1, sizeof(ast_move_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_MOVE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_MOVE_ */
