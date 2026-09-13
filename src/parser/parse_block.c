#include "parser/ast_block.h"
#include "parser/parse_stmt.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

ast_node_t *parse_block(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_symbol(p, "{")) return NULL;
    advance(p);
    skip_trivia(p);

    ast_node_t *stmts = NULL, *stmts_last = NULL;

    while (!check_symbol(p, "}")) {
        ast_node_t *stmt = parse_stmt(p);
        if (!stmt || stmt->kind == AST_ERROR) {
            if (!stmt) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected statement in block");
            }
            return stmt;
        }
        ast_append(&stmts, &stmts_last, NULL, stmt);
        skip_trivia(p);
    }

    if (!expect_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '}' to close block");
    }

    ast_node_t *node = ast_block_new(p->arena, tb, p->pos);
    ((ast_block_t *)node)->stmts      = stmts;
    ((ast_block_t *)node)->stmts_last = stmts_last;
    return node;
}
