#ifndef _H_CLUX_PARSER_AST_PTR_
#define _H_CLUX_PARSER_AST_PTR_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/lexer.h"
#include <stdbool.h>

/* 指针类型：`*T`（裸指针）。base_type 为被指向类型表达式。
 * 嵌套递归表达递归指针（`**T` 等）。 */
typedef struct {
    ast_node_t  base;
    ast_node_t *base_type;  /* 被指向类型表达式 T */
} ast_ptr_t;

static inline ast_node_t *ast_ptr_new(arena_t *arena,
                                      uint32_t tok_begin, uint32_t tok_end) {
    ast_ptr_t *n = (ast_ptr_t *)arena_calloc(
        arena, 1, sizeof(ast_ptr_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_PTR;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_PTR_ */
