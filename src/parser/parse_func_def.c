#include "parser/ast_func_def.h"
#include "parser/ast_block.h"
#include "parser/ast_pack_type.h"
#include "parser/ast_var_def.h"
#include "parser/ast_func_type.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"
#include "parser/ast_node.h"
#include "core/strslice.h"
#include <string.h>

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

    /* 参数名：标识符（保留名：`__` 前缀 / 裸 `_` 拒绝） */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected parameter name");
    }
    {
        ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "parameter name");
        if (rerr) return rerr;
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* 类型标注：:type（必须，类型即表达式，普通表达式解析）。
       min_prec=1 限定：不消费逗号/右括号（调用者处理）。
       parse_scope_annotated_type 额外处理前导作用域标注（m3-design §5）。 */
    if (!expect_symbol(p, ":")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ':' and type after parameter name");
    }
    skip_trivia(p);

    /* M7 参数包参数：`args: ...Args`（必须是最后一个参数；parser 允许出现在
       任意位置，sema 校验位置）。 */
    if (check_symbol(p, "...")) {
        uint32_t pb = p->pos;
        advance(p);
        skip_trivia(p);
        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, pb, p->pos,
                                 "expected parameter pack name after '...'");
        }
        {
            ast_node_t *rerr = parse_reserved_ident_error(
                p, cur_token(p), "generic parameter pack");
            if (rerr) return rerr;
        }
        strslice_t pack_name = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);

        ast_node_t *pt = ast_pack_type_new(p->arena, pb, p->pos);
        if (!pt) return NULL;
        ((ast_pack_type_t *)pt)->name = pack_name;

        ast_node_t *node = ast_var_def_new(p->arena, tb, p->pos);
        ((ast_var_def_t *)node)->name      = name;
        ((ast_var_def_t *)node)->type_expr = pt;
        return node;
    }

    ast_node_t *type_expr = parse_scope_annotated_type(p);
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
    /* 所有权必须在类型槽位折叠前缓存（折叠后 token 丢失）——extern cfunc 的
       参数到 Pass 1c 复审 C ABI 约束时还要用它（`own *T` 参数必须被拒）。 */
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
    return parse_scope_annotated_type(p);
    return parse_unary(p);
}

ast_node_t *parse_func_param_decl(parser_t *p) { return parse_param(p); }

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
        {
            ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "capture name");
            if (rerr) return rerr;
        }
        strslice_t name = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);

        /* 可选类型标注：:type（min_prec=1 限定，不消费 '=' 与逗号/右括号） */
        ast_node_t *type_expr = NULL;
        if (check_symbol(p, ":")) {
            advance(p);
            skip_trivia(p);
            type_expr = true
                            ? parse_raw_type_atom(p)
                            : parse_expr_prec(p, 1);
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

    /* 类型注解形态：`name: Type`（闭包类型捕获槽位；函数字面量的无初始值
       类型捕获也可用，语义由 sema 决定）。 */
    ast_node_t *type_expr = NULL;
    if (check_symbol(p, ":")) {
        advance(p);
        skip_trivia(p);
        p->stop_at_pipe++;
        type_expr = true
                        ? parse_raw_type_atom(p)
                        : parse_expr_prec(p, 1);
        p->stop_at_pipe--;
        if (!type_expr || type_expr->kind == AST_ERROR) {
            if (!type_expr) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected type after ':' in capture");
            }
            return type_expr;
        }
        skip_trivia(p);
    }

    ast_node_t *node = ast_var_def_new(p->arena, tb, p->pos);
    ((ast_var_def_t *)node)->name = name;
    ((ast_var_def_t *)node)->type_expr = type_expr;
    return node;
}

/** 解析捕获列表：| item (, item)* |，空列表 || 合法（返回 NULL）。
 *  非 static：defer 语句（parse_defer.c）复用同一份捕获列表解析——
 *  捕获项的两种形态与闭包完全一致，不另造一套。 */
ast_node_t *parse_capture_list(parser_t *p) {
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

ast_node_t *parse_func_like_ex(parser_t *p, ast_kind_t expected_kind,
                                     const char *kw, strslice_t pre_name,
                                     strslice_t pre_lib) {
    uint32_t tb = p->pos;

    if (kw) { if (!check_keyword(p, kw)) { p->pos = tb; return NULL; }
        advance(p);  /* commit；kw == NULL = 调用方已消费关键字 */
    }
    skip_trivia(p);

    /* 闭包作用域标注（m3-design §9）：func '<x> |...| —— 标注在 func 后、
       | 前。捕获 ref/opaque 时强制标注（视同 struct ref 字段）。'< 是
       lexer 合成的复合 SYMBOL token（仅在有捕获列表时有意义）。 */
    strslice_t *scope_annot_names = NULL;
    size_t scope_annot_count = 0;
    if (check_symbol(p, "'<")) {
        advance(p); /* 消费 '< */
        skip_trivia(p);
        scope_annot_names = parse_scope_annotation(p, &scope_annot_count);
        if (!scope_annot_names) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "invalid scope annotation on closure");
        }
    }

    /* 捕获列表：func |a,(b:i32 = c+d)| name(...) —— 紧跟 func 之后。
       仅函数定义/字面量（AST_FUNC_DEF）允许；函数类型（'->' 分支）拒绝，
       在 '->' 分支处检查。空列表 || 合法（返回 NULL = 无捕获）。 */
    ast_node_t *captures = NULL;
    if (pre_lib.len > 0 && check_symbol(p, "|")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "cfunc cannot capture (C ABI has no closures)");
    }
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
    strslice_t name = pre_name.len > 0 ? pre_name : STRSLICE_EMPTY;
    if (pre_name.len == 0 &&
        check_kind(p, TOKEN_TYPE_IDENTIFIER) && !check_symbol(p, "(")) {
        {
            ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "function name");
            if (rerr) return rerr;
        }
        name = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);
    }

    /* M7 泛型参数列表：紧跟声明关键字/名字之后、参数列表之前。
     *   func f[T, U](...)      语句级定义
     *   func[T](...)           函数字面量
     * 函数类型（'->' 分支）不允许泛型参数（多态签名不是 clux 类型）；
     * cfunc / extern cfunc 保持非泛型（C 无泛型，设计 §22）。 */
    ast_generic_params_t generic;
    memset(&generic, 0, sizeof generic);
    if (check_symbol(p, "[")) {
        ast_node_t *gerr = parse_generic_params(p, &generic);
        if (gerr) return gerr;
        if ((kw && strcmp(kw, "cfunc") == 0) || pre_lib.len > 0) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "cfunc cannot be generic (the C ABI has no "
                                 "templates)");
        }
        /* 泛型函数字面量 + 捕获列表：允许 `func[T] |caps| (...)`
         * （捕获列表也可以写在 func 之后、泛型参数之前，两条路径都支持）。 */
        if (!captures && check_symbol(p, "|")) {
            captures = parse_capture_list(p);
            if (captures && captures->kind == AST_ERROR) return captures;
            skip_trivia(p);
        }
    }

    /* 参数列表：(param_item, param_item, ...)，参数项形态统一解析 */
    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'func'");
    }
    skip_trivia(p);

    bool variadic = false;
    if (check_symbol(p, "...")) {
        if (pre_lib.len == 0) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "variadic ... is only allowed in extern cfunc");
        }
        variadic = true;
        advance(p);
        skip_trivia(p);
    }

    /* M6 FFI：...（C 可变参数）只在 extern cfunc 中允许 */
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
            /* M6 FFI：`, ...` —— 可变参数只允许出现在参数表末尾 */
            if (check_symbol(p, "...")) {
                if (pre_lib.len == 0) {
                    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                         "variadic ... is only allowed in extern cfunc");
                }
                variadic = true;
                advance(p);
                skip_trivia(p);
                break;
            }

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
        /* 多态签名不是 clux 类型：`func[T](...) -> T` 非法（泛型只属于
           函数定义/字面量）。 */
        if (generic.count > 0) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "function type cannot be generic (a polymorphic "
                                 "signature is not a clux type)");
        }
        /* 捕获列表不属于函数类型（签名擦除闭包，类型无捕获概念） */
        /* 闭包类型：`func |a: T, b: U| (params) -> R`。捕获槽位必须带类型
           （类型里没有初始值/推断的位置）。 */
        for (ast_node_t *c = captures; c; c = c->next) {
            if (c->kind != AST_VAR_DEF) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "closure type captures must be "
                                     "name: type declarations");
            }
            ast_var_def_t *cv = (ast_var_def_t *)c;
            if (!cv->type_expr) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "closure type capture must have a type "
                                     "(write `name: Type`)");
            }
            if (cv->init && cv->init->kind != AST_UNDEF) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "closure type capture cannot have an "
                                     "initializer");
            }
        }
        /* 作用域标注不属于函数类型（标注约束闭包捕获，类型无捕获） */
        if (scope_annot_count > 0) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "function type cannot have a scope annotation");
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
        ast_node_t *return_type = parse_scope_annotated_type(p);
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
        ((ast_func_type_t *)node)->captures    = captures;
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

        ast_node_t *return_expr = parse_scope_annotated_type(p);
        if (!return_expr || return_expr->kind == AST_ERROR) {
            if (!return_expr) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected return type after ':' in function literal");
            }
            return return_expr;
        }

        ast_node_t *body = NULL;
    if (pre_lib.len > 0) {
        if (!expect_symbol(p, ";")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ';' after extern cfunc declaration");
        }
    } else {
        body = parse_block(p);
    }
        if (pre_lib.len == 0 && (!body || body->kind == AST_ERROR)) {
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
        fn->scope_annot_names = scope_annot_names;
        fn->scope_annot_count = scope_annot_count;
        fn->params      = params;
        fn->params_last = params_last;
        fn->return_expr = return_expr;
        fn->body        = body;
    fn->is_variadic = variadic;
        fn->generic     = generic;
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

    ast_node_t *return_expr = parse_scope_annotated_type(p);
    if (!return_expr || return_expr->kind == AST_ERROR) {
        if (!return_expr) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected return type after ':'");
        }
        return return_expr;
    }

    ast_node_t *body = NULL;
    if (pre_lib.len > 0) {
        if (!expect_symbol(p, ";")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ';' after extern cfunc declaration");
        }
    } else {
        body = parse_block(p);
    }
    if (pre_lib.len == 0 && (!body || body->kind == AST_ERROR)) {
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
    fn->scope_annot_names = scope_annot_names;
    fn->scope_annot_count = scope_annot_count;
    fn->params      = params;
    fn->params_last = params_last;
    fn->return_expr = return_expr;
    fn->body        = body;
    fn->is_variadic = variadic;
    fn->generic     = generic;
    return node;
}

ast_node_t *parse_func_def(parser_t *p) {
    return parse_func_like(p, AST_FUNC_DEF, "func");
}

/* ---- M6 FFI：cfunc 定义（C ABI，有函数体）---- */

/** 解析 `cfunc <name>(<params>): <ret> { <body> }`
 *
 * 与 `func` 同构（复用 parse_func_like，只把关键字换成 "cfunc"），额外置
 * `is_cfunc`：语义走 VM 执行，但符号与调用约定是 C ABI，可被动态库回调
 * （Phase 5）。捕获列表 `|...|` 对 cfunc 非法——调用方不会解析它，出现即语法错误。
 */
ast_node_t *parse_cfunc_def(parser_t *p) {
    ast_node_t *fn = parse_func_like(p, AST_FUNC_DEF, "cfunc");
    if (fn && fn->kind == AST_FUNC_DEF)
        ((ast_func_def_t *)fn)->is_cfunc = true;
    return fn;
}

/* ---- 兼容包装：3 参数版（关键字必给，无预解析）---- */

ast_node_t *parse_func_like(parser_t *p, ast_kind_t expected_kind,
                            const char *kw) {
    return parse_func_like_ex(p, expected_kind, kw, STRSLICE_EMPTY,
                              STRSLICE_EMPTY);
}

/* ---- M6 FFI：extern cfunc <库>::<名>(<参数>): <返回>; ---- */

/** 解析 `extern cfunc <lib>::<name>(params): ret;`（调用方已消费 `extern`）。
 * 无函数体：符号在运行期由 dlsym/GetProcAddress 从库句柄解析（Phase 3/4）。
 * `...` 允许（C 可变参数）；捕获列表非法（C 无闭包）。
 * `<lib>` 是 `extern lib <lib> from "..."` 声明的别名；库名 `"*"` 表示
 * **当前进程自身**（已链接进来的符号，见 ffi_lib.c）。
 *
 * 只有 `cfunc` 能出现在 `extern` 后面：clux `func` 是 env-first 调用约定，
 * 不能作为 C ABI（`extern func` 在 parser.c 里明确报错）。 */
ast_node_t *parse_extern_cfunc(parser_t *p, uint32_t tb) {
    if (!expect_keyword(p, "cfunc")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected 'cfunc' after 'extern'");
    }
    skip_trivia(p);
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected library alias in extern cfunc <lib>::<name> "
                             "(alias comes from `extern lib <alias> from \"<name>\";`)");
    }
    strslice_t lib = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);
    if (!expect_symbol(p, "::")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected :: in extern cfunc <lib>::<name>");
    }
    skip_trivia(p);
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected function name after ::");
    }
    strslice_t fname = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    ast_node_t *fn = parse_func_like_ex(p, AST_FUNC_DEF, NULL, fname, lib);
    if (fn && fn->kind == AST_FUNC_DEF) {
        ast_func_def_t *fd = (ast_func_def_t *)fn;
        fd->is_extern_cfunc = true;
        fd->lib_name        = lib;
    }
    return fn;
}

/* ---- M6 FFI：extern var <库>::<名>: <类型>; ---- */

/** 解析 `extern var <lib>::<name>: <T>;`（调用方已消费 `extern`）。
 *
 * 动态库导出的**全局数据符号**：运行期 dlsym/GetProcAddress 得到变量地址，
 * 读写都直接作用于库里的那块内存（没有本地副本），因此：
 *   - 无初始化器（`init = NULL`）——语法上是声明，`= ...` 一律语法错；
 *   - 类型标注**必须**给出（宽度/类别决定载入与写回的字节数，无法推断）；
 *   - 引用是**左值**：`crt::name = v;` 合法（对比 `extern cfunc` 只能调用）。 */
ast_node_t *parse_extern_var(parser_t *p, uint32_t tb) {
    if (!expect_keyword(p, "var")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected 'var' after 'extern'");
    }
    skip_trivia(p);
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected library alias in extern var <lib>::<name> "
                             "(alias comes from `extern lib <alias> from \"<name>\";`)");
    }
    strslice_t lib = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);
    if (!expect_symbol(p, "::")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected :: in extern var <lib>::<name>");
    }
    skip_trivia(p);
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected symbol name after ::");
    }
    strslice_t vname = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    if (!expect_symbol(p, ":")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ':' and type in extern var declaration");
    }
    skip_trivia(p);
    ast_node_t *type_expr = parse_scope_annotated_type(p);
    if (!type_expr || type_expr->kind == AST_ERROR) {
        if (!type_expr) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected type after ':' in extern var");
        }
        return type_expr;
    }
    skip_trivia(p);
    if (!check_symbol(p, ";")) {
        /* 用 check_symbol 自行判定：expect_symbol 会先报一条通用 "expected ';'"
           再返回 false，那样同一处会出两条诊断。 */
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after extern var declaration "
                             "(extern var has no initializer)");
    }
    advance(p);

    ast_node_t *node = ast_var_def_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ast_var_def_t *vd = (ast_var_def_t *)node;
    vd->name          = vname;
    vd->type_expr     = type_expr;
    /* 所有权缓存（折叠前）——extern var 到 Pass 1c 复审 ABI 时要用：
       `extern var <lib>::<名>: own *T` 必须被拒（C 无所有权）。 */
    vd->init          = NULL; /* 声明而非定义：库里已有那块内存 */
    vd->is_extern_var = true;
    vd->lib_name      = lib;
    return node;
}