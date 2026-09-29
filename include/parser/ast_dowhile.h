#ifndef _H_CLUX_PARSER_AST_DOWHILE_
#define _H_CLUX_PARSER_AST_DOWHILE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

typedef struct {
    ast_node_t  base;
    ast_node_t *cond;
    ast_node_t *body;        /* AST_BLOCK */
} ast_dowhile_t;

static inline ast_node_t *ast_dowhile_new(arena_t *arena,
                                          uint32_t tok_begin, uint32_t tok_end) {
    ast_dowhile_t *n = (ast_dowhile_t *)arena_calloc(
        arena, 1, sizeof(ast_dowhile_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_DOWHILE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 do { body } while (cond); */
ast_node_t *parse_dowhile(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
