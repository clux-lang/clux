#include "parser/ast_for.h"
#include "parser/ast_block.h"
#include "parser/ast_var_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_stmt.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

ast_node_t *parse_for(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "for")) return NULL;
    advance(p);
    skip_trivia(p);

    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'for'");
    }
    skip_trivia(p);

    /* init: var_def / assign_or_expr_stmt / 空(;)
     * var_def 和 assign_or_expr_stmt 都会消费末尾的 ; */
    ast_node_t *init = NULL;
    if (!check_symbol(p, ";")) {
        if (check_keyword(p, "var")) {
            init = parse_var_def(p);
        } else {
            init = parse_assign_or_expr_stmt(p);
        }
        if (init && init->kind == AST_ERROR) return init;
        if (!init) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected init statement in for loop");
        }
    } else {
        advance(p);  /* 消费空 init 的 ; */
    }
    skip_trivia(p);

    /* cond: 表达式 / 空 */
    ast_node_t *cond = NULL;
    if (!check_symbol(p, ";")) {
        cond = parse_expr(p);
        if (!cond || cond->kind == AST_ERROR) {
            if (!cond) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected condition in for loop");
            }
            return cond;
        }
    }
    skip_trivia(p);

    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after for condition");
    }
    skip_trivia(p);

    /* update: 表达式 / 空（赋值也是表达式）
     * for 的 update 后面是 ) 不是 ;，直接用 parse_expr */
    ast_node_t *update = NULL;
    if (!check_symbol(p, ")")) {
        update = parse_expr(p);
        if (!update || update->kind == AST_ERROR) {
            if (!update) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected expression in for update");
            }
            return update;
        }
    }
    skip_trivia(p);

    if (!expect_symbol(p, ")")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ')' after for clauses");
    }
    skip_trivia(p);

    ast_node_t *body = parse_block(p);
    if (!body || body->kind == AST_ERROR) {
        if (!body) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '{' after for clauses");
        }
        return body;
    }

    ast_node_t *node = ast_for_new(p->arena, tb, p->pos);
    ((ast_for_t *)node)->init   = init;
    ((ast_for_t *)node)->cond   = cond;
    ((ast_for_t *)node)->update = update;
    ((ast_for_t *)node)->body   = body;
    return node;
}
