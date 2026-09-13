#include "parser/ast_while.h"
#include "parser/ast_block.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

ast_node_t *parse_while(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "while")) return NULL;
    advance(p);
    skip_trivia(p);

    /* 条件必须用 () 包裹 */
    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'while'");
    }
    skip_trivia(p);

    ast_node_t *cond = parse_expr(p);
    if (!cond || cond->kind == AST_ERROR) {
        if (!cond) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected condition after '('");
        }
        return cond;
    }
    skip_trivia(p);

    if (!expect_symbol(p, ")")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ')' after while condition");
    }
    skip_trivia(p);

    ast_node_t *body = parse_block(p);
    if (!body || body->kind == AST_ERROR) {
        if (!body) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '{' after while condition");
        }
        return body;
    }

    ast_node_t *node = ast_while_new(p->arena, tb, p->pos);
    ((ast_while_t *)node)->cond = cond;
    ((ast_while_t *)node)->body = body;
    return node;
}
