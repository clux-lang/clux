#include "parser/ast_type_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

ast_node_t *parse_type_def(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "type")) return NULL;
    advance(p);
    skip_trivia(p);

    /* 类型名：标识符 */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected type name after 'type'");
    }
    {
        ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "type name");
        if (rerr) return rerr;
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* M7 泛型参数表：type Pair[T, U] = <T, U>; */
    ast_generic_params_t generic = {0};
    if (check_symbol(p, "[")) {
        ast_node_t *gerr = parse_generic_params(p, &generic);
        if (gerr) return gerr;
    }

    /* 类型表达式：= expr（必须） */
    if (!check_symbol(p, "=")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '=' in type definition");
    }
    advance(p);
    skip_trivia(p);

    bool raw_type = true && check_symbol(p, "*");
    ast_node_t *expr = raw_type ? parse_raw_type_atom(p)
                                : parse_expr(p);
    if (!expr || expr->kind == AST_ERROR) {
        if (!expr) {
            return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos,
                                 "expected expression after '=' in type definition");
        }
        return expr;
    }
    skip_trivia(p);

    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after type definition");
    }

    ast_node_t *node = ast_type_def_new(p->arena, tb, p->pos);
    ((ast_type_def_t *)node)->name = name;
    ((ast_type_def_t *)node)->expr = expr;
    ((ast_type_def_t *)node)->generic = generic;
    return node;
}
