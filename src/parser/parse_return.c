#include "parser/ast_return.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

ast_node_t *parse_return(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "return")) return NULL;
    advance(p);
    skip_trivia(p);

    ast_node_t *value = NULL;
    if (!check_symbol(p, ";")) {
        value = parse_expr(p);
        if (!value || value->kind == AST_ERROR) {
            if (!value) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected expression after 'return'");
            }
            return value;
        }
        skip_trivia(p);
    }

    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after return statement");
    }

    ast_node_t *node = ast_return_new(p->arena, tb, p->pos);
    ((ast_return_t *)node)->value = value;
    return node;
}
