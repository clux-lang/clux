#include "parser/ast_test.h"
#include "parser/ast_block.h"
#include "parser/ast_error.h"
#include "parser/ast_string_lit.h"
#include "parser/parse_utils.h"

/* 顶层 test "Suite.Name" { ... } */
ast_node_t *parse_test_block(parser_t *p) {
    uint32_t tb = p->pos;
    advance(p); /* test */
    skip_trivia(p);

    ast_node_t *name_node = parse_string_lit(p);
    if (!name_node || name_node->kind == AST_ERROR) {
        if (!name_node && p->has_error) return NULL;
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected test name string after 'test'");
    }
    strslice_t name = ((ast_string_lit_t *)name_node)->text;
    if (name.len == 0) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "test name must not be empty");
    }

    skip_trivia(p);
    ast_node_t *body = parse_block(p);
    if (!body || body->kind == AST_ERROR) {
        if (!body && p->has_error) return NULL;
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '{' after test name");
    }

    ast_node_t *node = ast_test_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_test_t *)node)->name = name;
    ((ast_test_t *)node)->body = body;
    return node;
}
