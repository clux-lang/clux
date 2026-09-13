#include "parser/ast_if.h"
#include "parser/ast_block.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

ast_node_t *parse_if(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "if")) return NULL;
    advance(p);
    skip_trivia(p);

    /* 条件必须用 () 包裹 */
    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'if'");
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
                             "expected ')' after if condition");
    }
    skip_trivia(p);

    ast_node_t *then_body = parse_block(p);
    if (!then_body || then_body->kind == AST_ERROR) {
        if (!then_body) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '{' after if condition");
        }
        return then_body;
    }
    skip_trivia(p);

    ast_node_t *else_body = NULL;
    if (check_keyword(p, "else")) {
        advance(p);
        skip_trivia(p);

        /* else if 链 */
        if (check_keyword(p, "if")) {
            else_body = parse_if(p);
        } else {
            else_body = parse_block(p);
        }
        if (!else_body || else_body->kind == AST_ERROR) {
            if (!else_body) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected block or 'if' after 'else'");
            }
            return else_body;
        }
    }

    ast_node_t *node = ast_if_new(p->arena, tb, p->pos);
    ((ast_if_t *)node)->cond      = cond;
    ((ast_if_t *)node)->then_body = then_body;
    ((ast_if_t *)node)->else_body = else_body;
    return node;
}
