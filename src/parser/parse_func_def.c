#include "parser/ast_func_def.h"
#include "parser/ast_block.h"
#include "parser/ast_var_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"
#include "parser/ast_node.h"
#include "core/strslice.h"

/* ================================================================ */
/* parse_func_like: func 统一入口                                     */
/*                                                                  */
/* 上下文决定语义：                                                   */
/*   - parse_func_def (语句级) 调用 → 函数定义 → AST_FUNC_DEF        */
/*   - parse_primary (表达式级, M2+) 调用 → 匿名函数字面量            */
/*                                                                  */
/* 语句级：func name(params):type { body }                           */
/* 表达式级 (M2+)：func(params):type { body }   (无 name)            */
/* ================================================================ */

/** 解析参数列表中的单个参数：name:type → AST_VAR_DEF（无 init） */
static ast_node_t *parse_param(parser_t *p) {
    uint32_t tb = p->pos;

    /* 参数名：标识符 */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected parameter name");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* 类型标注：:type（必须，类型即表达式，普通表达式解析）。
       min_prec=1 限定：不消费逗号/右括号（调用者处理）。 */
    if (!expect_symbol(p, ":")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ':' and type after parameter name");
    }
    skip_trivia(p);

    ast_node_t *type_expr = parse_expr_prec(p, 1);
    if (!type_expr || type_expr->kind == AST_ERROR) {
        if (!type_expr) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected type after ':'");
        }
        return type_expr;
    }

    /* 注意：不消费逗号或 )，由调用者处理 */

    ast_node_t *node = ast_var_def_new(p->arena, tb, p->pos);
    ((ast_var_def_t *)node)->name      = name;
    ((ast_var_def_t *)node)->type_expr = type_expr;
    /* init 为 NULL：参数定义无初始化表达式 */
    return node;
}

ast_node_t *parse_func_like(parser_t *p, ast_kind_t expected_kind) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "func")) { p->pos = tb; return NULL; }
    advance(p);  /* commit: 消费 "func" */
    skip_trivia(p);

    /*
     * 分歧点：func 后跟标识符且不紧跟 ( → 语句级函数定义（有 name）
     *         func 后紧跟 ( → 表达式级匿名函数（无 name，M2+）
     *
     * 判断方式：func 后当前 token 是 IDENTIFIER 且下一个不是 (
     */
    strslice_t name = STRSLICE_EMPTY;
    if (check_kind(p, TOKEN_TYPE_IDENTIFIER) && !check_symbol(p, "(")) {
        name = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);
    }

    /* 参数列表：(name:type, name:type, ...) */
    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'func'");
    }
    skip_trivia(p);

    ast_node_t *params      = NULL;
    ast_node_t *params_last = NULL;

    if (!check_symbol(p, ")")) {
        ast_node_t *param = parse_param(p);
        if (!param || param->kind == AST_ERROR) {
            if (!param) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected parameter in function parameter list");
            }
            return param;
        }
        ast_append(&params, &params_last, NULL, param);
        skip_trivia(p);

        while (check_symbol(p, ",")) {
            advance(p);   /* 消费 , */
            skip_trivia(p);

            param = parse_param(p);
            if (!param || param->kind == AST_ERROR) {
                if (!param) {
                    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                         "expected parameter after ','");
                }
                return param;
            }
            ast_append(&params, &params_last, NULL, param);
            skip_trivia(p);
        }
    }

    if (!expect_symbol(p, ")")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ')' after function parameters");
    }
    skip_trivia(p);

    if (expected_kind == AST_FUNC_DEF) {
        /* 语句级函数定义：必须有 name */
        if (name.len == 0) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected function name after 'func'");
        }

        /* 可选返回类型：:type（省略 = void；类型即表达式，普通表达式解析） */
        ast_node_t *return_expr = NULL;
        if (check_symbol(p, ":")) {
            advance(p);
            skip_trivia(p);

            return_expr = parse_expr_prec(p, 1);
            if (!return_expr || return_expr->kind == AST_ERROR) {
                if (!return_expr) {
                    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                         "expected return type after ':'");
                }
                return return_expr;
            }
        }

        /* 函数体：{ ... } */
        ast_node_t *body = parse_block(p);
        if (!body || body->kind == AST_ERROR) {
            if (!body) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected '{' for function body");
            }
            return body;
        }

        ast_node_t *node = ast_func_def_new(p->arena, tb, p->pos);
        ast_func_def_t *fn = (ast_func_def_t *)node;
        fn->name        = name;
        fn->params      = params;
        fn->params_last = params_last;
        fn->return_expr = return_expr;
        fn->body        = body;
        return node;
    }

    /* M2+: 表达式级匿名函数字面量 */
    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                         "func literal not supported in M1");
}

ast_node_t *parse_func_def(parser_t *p) {
    return parse_func_like(p, AST_FUNC_DEF);
}
