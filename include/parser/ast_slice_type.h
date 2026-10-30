#ifndef _H_CLUX_PARSER_AST_SLICE_TYPE_
#define _H_CLUX_PARSER_AST_SLICE_TYPE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/lexer.h"
#include <stdbool.h>

/* 切片类型：`[]T`。elem_type 为元素类型表达式 T。 */
typedef struct {
    ast_node_t  base;
    ast_node_t *elem_type;  /* 元素类型表达式 T */
} ast_slice_type_t;

static inline ast_node_t *ast_slice_type_new(arena_t *arena,
                                             uint32_t tok_begin,
                                             uint32_t tok_end) {
    ast_slice_type_t *n = (ast_slice_type_t *)arena_calloc(
        arena, 1, sizeof(ast_slice_type_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_SLICE_TYPE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_SLICE_TYPE_ */
