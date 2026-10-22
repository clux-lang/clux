#ifndef _H_CLUX_PARSER_AST_TRY_
#define _H_CLUX_PARSER_AST_TRY_

#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/* struct result 的 `.?' / `.!' 表达式（sema 生成，不经过 parser）：
 *
 *   { var temp = <setup.init>;        // setup 是 AST_VAR_DEF
 *     ok_call ? value_call : <tail_stmt> }
 *
 * tail_stmt 是 AST_RETURN（.?）或 AST_EXPR_STMT(panic)（.!）。
 * compiler 生成表达式内 early RET；成功路径留下一个值。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *setup;       /* AST_VAR_DEF：临时变量 + 原左值表达式 */
    ast_node_t *ok_call;
    ast_node_t *value_call;
    ast_node_t *tail_stmt;
    bool        panic_mode;
    bool        materialize;  /* copyable 结果离开临时前物化 */
} ast_try_t;

static inline ast_node_t *ast_try_new(arena_t *arena, uint32_t tok_begin,
                                      uint32_t tok_end) {
    ast_try_t *n = (ast_try_t *)arena_calloc(arena, 1, sizeof(ast_try_t),
                                             ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind = AST_TRY;
    n->base.tok_begin = tok_begin;
    n->base.tok_end = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif
