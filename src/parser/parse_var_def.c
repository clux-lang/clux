#include "parser/ast_var_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

ast_node_t *parse_var_def(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "var")) return NULL;
    advance(p);
    skip_trivia(p);

    /* 变量名：标识符 */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected variable name after 'var'");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* 可选类型标注：:type（类型即表达式，普通表达式解析）。
       用 min_prec=1 限定：不消费赋值（ASSIGN_LEFT_PREC=0）与逗号/右括号。 */
    ast_node_t *type_expr = NULL;
    if (check_symbol(p, ":")) {
        advance(p);
        skip_trivia(p);

        type_expr = parse_expr_prec(p, 1);
        if (!type_expr || type_expr->kind == AST_ERROR) {
            if (!type_expr) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected type after ':'");
            }
            return type_expr;
        }
    }

    /* 初始化表达式：= expr（必须） */
    if (!check_symbol(p, "=")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '=' and initializer in var definition");
    }
    advance(p);
    skip_trivia(p);

    ast_node_t *init = parse_expr(p);
    if (!init || init->kind == AST_ERROR) {
        if (!init) {
            /* 指向 '=' 之后实际无法解析的 token（parse_expr 失败时不前移） */
            return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos,
                                 "expected expression after '=' in var definition");
        }
        return init;
    }
    skip_trivia(p);

    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after var definition");
    }

    ast_node_t *node = ast_var_def_new(p->arena, tb, p->pos);
    ((ast_var_def_t *)node)->name      = name;
    ((ast_var_def_t *)node)->type_expr = type_expr;
    ((ast_var_def_t *)node)->init      = init;
    return node;
}
