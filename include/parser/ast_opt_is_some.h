#ifndef _H_CLUX_PARSER_AST_OPT_IS_SOME_
#define _H_CLUX_PARSER_AST_OPT_IS_SOME_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "core/arena.h"
#include <stdbool.h>

/**
 * AST_OPT_IS_SOME：`<object> != nil` / `<object> == nil` 的**直接形态**
 * （sema 生成，用于 `own ?T` 的销毁序列 `if (v != nil) { <inner 销毁序列> }`）。
 *
 * 为什么不复用 AST_BINARY：`!=` 的 op 是 `token_t*`，sema 无法合成 token
 * （与 `is` 同一问题，见 AST_IS_TAG）。
 *
 * 语义：编译 object → `OPT_IS_NONE`（压 bool：none=true）→ 需要 "some" 时
 * 再发 `NOT`。即 `want_some=true` 对应源码 `v != nil`。
 *
 * 不出现在用户源码中——只由 sema 的销毁序列生成器产生（`is_generated`）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *object;    /* ?T 值表达式 */
    bool        want_some; /* true → `v != nil`；false → `v == nil` */
} ast_opt_is_some_t;

static inline ast_node_t *ast_opt_is_some_new(arena_t *arena,
                                              uint32_t tok_begin,
                                              uint32_t tok_end) {
    ast_opt_is_some_t *n = (ast_opt_is_some_t *)arena_calloc(
        arena, 1, sizeof(ast_opt_is_some_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_OPT_IS_SOME;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    n->base.is_generated = true;  /* Pass 3b 不回走（见 stmt.c walk_stmt） */
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_OPT_IS_SOME_ */
