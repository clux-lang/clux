#ifndef _H_CLUX_PARSER_AST_TEST_
#define _H_CLUX_PARSER_AST_TEST_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

/* 顶层测试块：
 *   test "Suite.Name" { ... }
 * 普通 build/run 在 sema 前丢弃；测试模式转为无参 void 函数。 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;   /* "Suite.Name"（已展开转义） */
    ast_node_t *body;   /* AST_BLOCK */
} ast_test_t;

static inline ast_node_t *ast_test_new(arena_t *arena, uint32_t tok_begin,
                                       uint32_t tok_end) {
    ast_test_t *n = (ast_test_t *)arena_calloc(arena, 1, sizeof(ast_test_t),
                                               ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind = AST_TEST;
    n->base.tok_begin = tok_begin;
    n->base.tok_end = tok_end;
    return &n->base;
}

ast_node_t *parse_test_block(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
