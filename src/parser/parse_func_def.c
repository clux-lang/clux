#include "parser/ast_func_def.h"
#include "parser/ast_block.h"
#include "parser/ast_var_def.h"
#include "parser/ast_func_type.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"
#include "parser/ast_node.h"
#include "core/strslice.h"

/* ================================================================ */
/* parse_func_like: func 统一生成式入口                               */
/*                                                                  */
/* 生成式：                                                          */
/*   func [name] '(' params ')'                                     */
/*       ( ':' type '{' body '}' )   -- 函数定义 → AST_FUNC_DEF     */
/*     | ( '->' type )               -- 函数类型 → AST_FUNC_TYPE    */
/*                                                                  */
/* ')' 之后按 ':' / '->' 分叉：                                      */
/*   ':' → 函数定义（语句级, expected_kind=AST_FUNC_DEF）            */
/*   '->' → 函数类型（表达式级, expected_kind=AST_FUNC_TYPE）        */
/*                                                                  */
/* 参数项统一解析：IDENTIFIER 后跟 ':' → 具名参数（AST_VAR_DEF）；    */
/*   否则 → 纯类型表达式。分叉后校验参数形态：                        */
/*   - 函数定义要求全具名（name:type）                               */
/*   - 函数类型要求全纯类型（type）                                  */
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

/** 参数项统一解析：IDENTIFIER 后跟 ':' → 具名参数（AST_VAR_DEF）；
 *  否则 → 纯类型表达式（函数类型参数）。peek pos+1 判定。 */
static ast_node_t *parse_func_param_item(parser_t *p) {
    if (check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        const token_t *nt = (const token_t *)vec_get(p->tokens, p->pos + 1);
        if (nt && token_get_kind(nt) == TOKEN_TYPE_SYMBOL &&
            token_strslice(nt).len == 1 && token_strslice(nt).ptr[0] == ':') {
            return parse_param(p);
        }
    }
    return parse_unary(p);
}

/** 解析捕获列表中的单个捕获项 → AST_VAR_DEF：
 *   - 纯 id：name（type_expr/init 均为 NULL；类型取自外层符号）
 *   - 括号 VALUE DECL：(name[:type] = init)（临时构造：type 可选、init 必选，
 *     定义点在外层作用域求值后存入 closure_scope） */
static ast_node_t *parse_capture_item(parser_t *p) {
    uint32_t tb = p->pos;

    /* 括号形态：(name[:type] = init) */
    if (match_symbol(p, "(")) {
        skip_trivia(p);

        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected capture name after '('");
        }
        strslice_t name = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);

        /* 可选类型标注：:type（min_prec=1 限定，不消费 '=' 与逗号/右括号） */
        ast_node_t *type_expr = NULL;
        if (check_symbol(p, ":")) {
            advance(p);
            skip_trivia(p);
            type_expr = parse_expr_prec(p, 1);
            if (!type_expr || type_expr->kind == AST_ERROR) {
                if (!type_expr) {
                    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                         "expected type after ':' in capture");
                }
                return type_expr;
            }
        }

        /* 初始化：= expr（必须） */
        if (!expect_symbol(p, "=")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '=' and initializer in capture");
        }
        skip_trivia(p);

        ast_node_t *init = parse_expr_prec(p, 1);
        if (!init || init->kind == AST_ERROR) {
            if (!init) {
                return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos,
                                     "expected expression after '=' in capture");
            }
            return init;
        }

        if (!expect_symbol(p, ")")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ')' to close capture");
        }
        skip_trivia(p);

        ast_node_t *node = ast_var_def_new(p->arena, tb, p->pos);
        ((ast_var_def_t *)node)->name      = name;
        ((ast_var_def_t *)node)->type_expr = type_expr;
        ((ast_var_def_t *)node)->init      = init;
        return node;
    }

    /* 纯 id 捕获：name（type_expr/init 均为 NULL） */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected capture name or '(name = init)' in capture list");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    ast_node_t *node = ast_var_def_new(p->arena, tb, p->pos);
    ((ast_var_def_t *)node)->name = name;
    return node;
}

/** 解析捕获列表：| item (, item)* |，空列表 || 合法（返回 NULL）。 */
static ast_node_t *parse_capture_list(parser_t *p) {
    uint32_t tb = p->pos;
    advance(p);   /* 消费开 '|' */
    skip_trivia(p);

    ast_node_t *caps      = NULL;
    ast_node_t *caps_last = NULL;

    if (!check_symbol(p, "|")) {
        ast_node_t *cap = parse_capture_item(p);
        if (!cap || cap->kind == AST_ERROR) {
            if (!cap) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected capture in capture list");
            }
            return cap;
        }
        ast_append(&caps, &caps_last, NULL, cap);
        skip_trivia(p);

        while (check_symbol(p, ",")) {
            advance(p);
            skip_trivia(p);
            cap = parse_capture_item(p);
            if (!cap || cap->kind == AST_ERROR) {
                if (!cap) {
                    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                         "expected capture after ','");
                }
                return cap;
            }
            ast_append(&caps, &caps_last, NULL, cap);
            skip_trivia(p);
        }
    }

    if (!expect_symbol(p, "|")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '|' to close capture list");
    }
    skip_trivia(p);
    return caps;
}

ast_node_t *parse_func_like(parser_t *p, ast_kind_t expected_kind) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "func")) { p->pos = tb; return NULL; }
    advance(p);  /* commit: 消费 "func" */
    skip_trivia(p);

    /* 捕获列表：func |a,(b:i32 = c+d)| name(...) —— 紧跟 func 之后。
       仅函数定义/字面量（AST_FUNC_DEF）允许；函数类型（'->' 分支）拒绝，
       在 '->' 分支处检查。空列表 || 合法（返回 NULL = 无捕获）。 */
    ast_node_t *captures = NULL;
    if (check_symbol(p, "|")) {
        captures = parse_capture_list(p);
        if (captures && captures->kind == AST_ERROR) {
            return captures;
        }
    }

    /*
     * 分歧点：func 后跟标识符且不紧跟 ( → 语句级函数定义（有 name）
     *         func 后紧跟 ( → 表达式级（函数类型 / 匿名字面量，无 name）
     *
     * 判断方式：func 后当前 token 是 IDENTIFIER 且下一个不是 (
     */
    strslice_t name = STRSLICE_EMPTY;
    if (check_kind(p, TOKEN_TYPE_IDENTIFIER) && !check_symbol(p, "(")) {
        name = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);
    }

    /* 参数列表：(param_item, param_item, ...)，参数项形态统一解析 */
    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'func'");
    }
    skip_trivia(p);

    ast_node_t *params      = NULL;
    ast_node_t *params_last = NULL;

    if (!check_symbol(p, ")")) {
        ast_node_t *param = parse_func_param_item(p);
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

            param = parse_func_param_item(p);
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

    /* ---- 分叉：')' 之后按 ':' / '->' 判定函数定义 or 函数类型 ---- */

    /* 函数类型：func(params)->type（表达式级） */
    if (check_symbol(p, "->")) {
        if (expected_kind != AST_FUNC_TYPE) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "function type not allowed here (expected ':' and body)");
        }
        /* 捕获列表不属于函数类型（签名擦除闭包，类型无捕获概念） */
        if (captures) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "function type cannot have a capture list");
        }
        /* 参数必须是纯类型表达式（无具名） */
        for (ast_node_t *pr = params; pr; pr = pr->next) {
            if (pr->kind == AST_VAR_DEF) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "function type parameters must be types, not named");
            }
        }
        advance(p);
        skip_trivia(p);
        ast_node_t *return_type = parse_unary(p);
        if (!return_type || return_type->kind == AST_ERROR) {
            if (!return_type) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected return type after '->' in function type");
            }
            return return_type;
        }
        ast_node_t *node = ast_func_type_new(p->arena, tb, p->pos);
        ((ast_func_type_t *)node)->params      = params;
        ((ast_func_type_t *)node)->params_last = params_last;
        ((ast_func_type_t *)node)->return_type = return_type;
        return node;
    }

    /* 函数定义 / 函数字面量：func [name](params):type { body } */
    if (expected_kind == AST_FUNC_TYPE) {
        /* 表达式级：func(...) 后既无 '->'，即函数字面量（函数值）。
           与语句级函数定义共享 AST_FUNC_DEF 节点——语义差异（不注册
           作用域名字、不提升）推迟到 sema/compiler 消费层：表达式内的
           AST_FUNC_DEF 一律按字面量处理，name 可有可无（有则仅作函数
           显示名，不绑定符号）。参数必须具名。 */
        for (ast_node_t *pr = params; pr; pr = pr->next) {
            if (pr->kind != AST_VAR_DEF) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "function literal parameters must have names");
            }
        }

        /* 返回类型：: type 必选（与函数定义一致，不允许隐式 void） */
        if (!expect_symbol(p, ":")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ':' and return type in function literal");
        }
        skip_trivia(p);

        ast_node_t *return_expr = parse_expr_prec(p, 1);
        if (!return_expr || return_expr->kind == AST_ERROR) {
            if (!return_expr) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected return type after ':' in function literal");
            }
            return return_expr;
        }

        /* 函数体：{ ... } */
        ast_node_t *body = parse_block(p);
        if (!body || body->kind == AST_ERROR) {
            if (!body) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected '{' for function literal body");
            }
            return body;
        }

        ast_node_t *node = ast_func_def_new(p->arena, tb, p->pos);
        ast_func_def_t *fn = (ast_func_def_t *)node;
        fn->name        = name; /* 可为空（匿名）或为显示名（不绑作用域） */
        fn->captures    = captures;
        fn->params      = params;
        fn->params_last = params_last;
        fn->return_expr = return_expr;
        fn->body        = body;
        return node;
    }

    /* 语句级函数定义：必须有 name */
    if (name.len == 0) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected function name after 'func'");
    }
    /* 参数必须是具名参数（AST_VAR_DEF） */
    for (ast_node_t *pr = params; pr; pr = pr->next) {
        if (pr->kind != AST_VAR_DEF) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "function definition parameters must have names");
        }
    }

    /* 返回类型：: type 必选（不允许隐式 void 返回值） */
    if (!expect_symbol(p, ":")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ':' and return type in function definition");
    }
    skip_trivia(p);

    ast_node_t *return_expr = parse_expr_prec(p, 1);
    if (!return_expr || return_expr->kind == AST_ERROR) {
        if (!return_expr) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected return type after ':'");
        }
        return return_expr;
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
    fn->captures    = captures;
    fn->params      = params;
    fn->params_last = params_last;
    fn->return_expr = return_expr;
    fn->body        = body;
    return node;
}

ast_node_t *parse_func_def(parser_t *p) {
    return parse_func_like(p, AST_FUNC_DEF);
}
