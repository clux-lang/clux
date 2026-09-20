#include "parser/ast_enum_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

/* 解析枚举定义 enum Name:Underlying { Var = val, ... }
 *
 * 语法：enum 名字 : 底层类型 { 标识符 = 值表达式, ... }
 * - 底层类型必须显式写出（缺 ':' 报错）
 * - 每个 variant 必须显式写 '= 值'（缺值报错）
 * - variant 列表不能为空（{} 报错）
 */
ast_node_t *parse_enum_def(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "enum")) return NULL;
    advance(p);
    skip_trivia(p);

    /* 枚举名：标识符 */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected enum name after 'enum'");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* 底层类型：: expr（必须） */
    if (!check_symbol(p, ":")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ':' and underlying type in enum definition");
    }
    advance(p);
    skip_trivia(p);

    ast_node_t *underlying = parse_expr(p);
    if (!underlying || underlying->kind == AST_ERROR) {
        if (!underlying) {
            return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos,
                                 "expected underlying type expression after ':'");
        }
        return underlying;
    }
    skip_trivia(p);

    /* variant 列表：{ ... } */
    if (!expect_symbol(p, "{")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '{' to start enum variant list");
    }
    skip_trivia(p);

    ast_node_t *variants = NULL, *variants_last = NULL;

    /* 空 variant 列表非法：{} */
    if (check_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "enum variant list must not be empty");
    }

    for (;;) {
        /* variant 名：标识符 */
        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected variant name in enum definition");
        }
        uint32_t vb = p->pos;
        strslice_t vname = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);

        /* variant 值：= expr（必须显式） */
        if (!check_symbol(p, "=")) {
            return ast_error_new(p->diag, p->tokens, p->arena, vb, p->pos,
                                 "expected '= value' after enum variant name");
        }
        advance(p);
        skip_trivia(p);

        ast_node_t *val = parse_expr(p);
        if (!val || val->kind == AST_ERROR) {
            if (!val) {
                return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos,
                                     "expected value expression after '='");
            }
            return val;
        }
        skip_trivia(p);

        ast_node_t *vn = ast_enum_variant_new(p->arena, vb, p->pos);
        if (!vn) return NULL;
        ((ast_enum_variant_t *)vn)->name  = vname;
        ((ast_enum_variant_t *)vn)->value = val;
        ast_append(&variants, &variants_last, NULL, vn);

        /* 逗号分隔，} 结束 */
        if (check_symbol(p, ",")) {
            advance(p);
            skip_trivia(p);
            continue;
        }
        break;
    }

    if (!expect_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '}' to end enum variant list");
    }

    ast_node_t *node = ast_enum_def_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_enum_def_t *)node)->name            = name;
    ((ast_enum_def_t *)node)->underlying_type = underlying;
    ((ast_enum_def_t *)node)->variants        = variants;
    ((ast_enum_def_t *)node)->variants_last   = variants_last;
    return node;
}
