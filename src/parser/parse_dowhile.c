#include "parser/ast_dowhile.h"
#include "parser/ast_block.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

ast_node_t *parse_dowhile(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "do")) return NULL;
    advance(p);
    skip_trivia(p);

    ast_node_t *body = parse_block(p);
    if (!body || body->kind == AST_ERROR) {
        if (!body) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '{' after 'do'");
        }
        return body;
    }
    skip_trivia(p);

    if (!check_keyword(p, "while")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected 'while' after do body");
    }
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
                             "expected ')' after do-while condition");
    }
    skip_trivia(p);

    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after do-while");
    }

    ast_node_t *node = ast_dowhile_new(p->arena, tb, p->pos);
    ((ast_dowhile_t *)node)->cond = cond;
    ((ast_dowhile_t *)node)->body = body;
    return node;
}
