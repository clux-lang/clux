#ifndef _H_CLUX_PARSER_AST_TYPE_DEF_
#define _H_CLUX_PARSER_AST_TYPE_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>

typedef struct {
    ast_node_t  base;
    strslice_t  name;        /* 类型名 */
    ast_node_t *expr;        /* 类型表达式（sema 求值后折叠为 AST_TYPE_REF） */
} ast_type_def_t;

static inline ast_node_t *ast_type_def_new(arena_t *arena,
                                           uint32_t tok_begin, uint32_t tok_end) {
    ast_type_def_t *n = (ast_type_def_t *)arena_calloc(
        arena, 1, sizeof(ast_type_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_TYPE_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析类型定义 type name = <type-expr>; */
ast_node_t *parse_type_def(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
