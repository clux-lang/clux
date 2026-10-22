#include "parser/ast_generic_param.h"
#include "parser/ast_pattern.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"
#include <string.h>

/* ===========================================================================
 * 泛型参数列表解析（M7）
 *
 *     func f[T, U = i32, N: comptime u64, ...Args](...)
 *
 * 规则（docs/m7-design.md §1/§2/§3/§30.1）：
 *   - 参数包 `...Args` 必须最后，且每个声明最多一个；
 *   - comptime 值参数必须显式写 `N: comptime <type>`；
 *   - 默认值从右向左省略（本层只记录，顺序规则在 sema 检查）；
 *   - `extends` 结构约束留给 P6（此处明确报"尚未实现"，不解析半截 PATTERN）。
 * =========================================================================== */

ast_node_t *parse_generic_params(parser_t *p, ast_generic_params_t *out) {
    if (!p || !out) return NULL;
    uint32_t tb = p->pos;

    if (!check_symbol(p, "[")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '[' to start generic parameter list");
    }
    advance(p);
    skip_trivia(p);

    if (check_symbol(p, "]")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "generic parameter list cannot be empty");
    }

    memset(out, 0, sizeof(*out));

    for (;;) {
        uint32_t pb = p->pos;

        bool is_pack = false;
        if (check_symbol(p, "...")) {
            is_pack = true;
            advance(p);
            skip_trivia(p);
        }

        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, pb, p->pos,
                                 "expected generic parameter name");
        }
        {
            ast_node_t *rerr =
                parse_reserved_ident_error(p, cur_token(p), "generic parameter");
            if (rerr) return rerr;
        }
        strslice_t name = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);

        /* 同一参数表内名字不能重复（T, T 会让绑定/推导歧义）。 */
        for (ast_node_t *q = out->first; q; q = q->next) {
            ast_generic_param_t *prev = (ast_generic_param_t *)q;
            if (strslice_eq(prev->name, name)) {
                return ast_error_new(p->diag, p->tokens, p->arena, pb, p->pos,
                                     "duplicate generic parameter '%.*s'",
                                     (int)name.len, name.ptr);
            }
        }

        bool is_comptime = false;
        ast_node_t *type_expr = NULL;
        ast_node_t *constraint = NULL;
        ast_node_t *default_expr = NULL;

        /* 值参数：': comptime <type>'。类型参数可写 ':' 是非法形态，统一
           在这里给出可操作诊断（而不是让 parse_expr 在 'comptime' 上失败）。 */
        if (check_symbol(p, ":")) {
            advance(p);
            skip_trivia(p);
            if (!check_keyword(p, "comptime")) {
                return ast_error_new(
                    p->diag, p->tokens, p->arena, pb, p->pos,
                    "generic value parameter must be declared 'comptime': write "
                    "'%.*s: comptime u64' (type parameters have no ':' clause)",
                    (int)name.len, name.ptr);
            }
            is_comptime = true;
            advance(p);
            skip_trivia(p);

            type_expr = parse_scope_annotated_type(p);
            if (!type_expr || type_expr->kind == AST_ERROR) {
                if (!type_expr) {
                    return ast_error_new(p->diag, p->tokens, p->arena, pb,
                                         p->pos,
                                         "expected type after ': comptime'");
                }
                return type_expr;
            }
            skip_trivia(p);
        }

        /* 结构约束：T extends <pattern>（PATTERN 专用语法，见
           parser/ast_pattern.h）。comptime 值参数不允许 extends（设计 §27）。 */
        if (check_keyword(p, "extends")) {
            if (is_comptime) {
                return ast_error_new(p->diag, p->tokens, p->arena, pb, p->pos,
                                     "comptime value parameter '%.*s' cannot "
                                     "have an 'extends' constraint",
                                     (int)name.len, name.ptr);
            }
            advance(p);
            skip_trivia(p);
            constraint = parse_pattern(p);
            if (!constraint || constraint->kind == AST_ERROR) {
                if (!constraint)
                    return ast_error_new(p->diag, p->tokens, p->arena, pb,
                                         p->pos,
                                         "expected type pattern after 'extends'");
                return constraint;
            }
            skip_trivia(p);
        }

        /* 默认值：'= expr'（min_prec=1 不消费 '='，由这里显式处理；
           默认值表达式本身用 parse_expr_prec(1) 解析，遇到 ','/']' 停止）。 */
        if (check_symbol(p, "=")) {
            advance(p);
            skip_trivia(p);
            default_expr = parse_expr_prec(p, 1);
            if (!default_expr || default_expr->kind == AST_ERROR) {
                if (!default_expr) {
                    return ast_error_new(p->diag, p->tokens, p->arena, pb,
                                         p->pos,
                                         "expected default value after '='");
                }
                return default_expr;
            }
            skip_trivia(p);
        }

        ast_node_t *node = ast_generic_param_new(p->arena, pb, p->pos);
        if (!node) return NULL;
        ast_generic_param_t *gp = (ast_generic_param_t *)node;
        gp->name         = name;
        gp->is_comptime  = is_comptime;
        gp->is_pack      = is_pack;
        gp->type_expr    = type_expr;
        gp->constraint   = constraint;
        gp->default_expr = default_expr;
        ast_generic_params_add(out, node);

        if (check_symbol(p, ",")) {
            if (is_pack) {
                return ast_error_new(
                    p->diag, p->tokens, p->arena, pb, p->pos,
                    "generic parameter pack '%.*s' must be the last parameter",
                    (int)name.len, name.ptr);
            }
            advance(p);
            skip_trivia(p);
            if (check_symbol(p, "]")) {
                return ast_error_new(p->diag, p->tokens, p->arena, pb, p->pos,
                                     "trailing ',' is not allowed in a generic "
                                     "parameter list");
            }
            continue;
        }

        if (!check_symbol(p, "]")) {
            return ast_error_new(p->diag, p->tokens, p->arena, pb, p->pos,
                                 "expected ',' or ']' in generic parameter list");
        }
        advance(p);
        skip_trivia(p);
        break;
    }

    return NULL;
}
