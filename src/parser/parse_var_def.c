#include "parser/ast_var_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"
#include "parser/ast_ptr.h"
#include "parser/ast_owned_type.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_scope_annot.h"
#include <string.h>   /* memcmp */

/**
 * 从类型表达式提取顶层所有权编码（声明见 ast_var_def.h）。
 *
 * **必须在类型槽位折叠前调用**：`sema_resolve_type_slot` 会把 AST_PTR /
 * AST_SLICE_TYPE 折叠成 AST_TYPE_REF，此后 ownership token 不复存在。
 * 放在 .c 而非头文件内联，是因为需要 `token_strslice`（parse_utils）与
 * 三种类型节点的完整定义。
 */

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
    {
        ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "variable name");
        if (rerr) return rerr;
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* 可选类型标注：:type（类型即表达式，普通表达式解析）。
       用 min_prec=1 限定：不消费赋值（ASSIGN_LEFT_PREC=0）与逗号/右括号。
       parse_scope_annotated_type 额外处理前导作用域标注 '<a,b,c>
       （m3-design §5：标注前导紧跟 ':' 之后，绑定在变量上）。 */
    ast_node_t *type_expr = NULL;
    if (check_symbol(p, ":")) {
        advance(p);
        skip_trivia(p);

        type_expr = parse_scope_annotated_type(p);
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
    /* 所有权必须在类型槽位折叠前缓存（折叠后 ownership token 丢失）——
       extern 声明（extern cfunc/var）到 Pass 1c 复审 ABI 时还要用它。 */
    ((ast_var_def_t *)node)->init      = init;
    return node;
}
