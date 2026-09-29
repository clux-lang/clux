#include "parser/ast_cunion_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_error.h"

/* 解析 C 语义 union 定义 cunion Name { field: type; ... }
 *
 * 与 struct 同构语法：字段直接平铺，每个字段即一个 member（字段名 = member
 * 名，字段类型 = member 类型）。
 * 语法：cunion 名字 { 标识符 : 类型表达式 ; ... }
 * - 字段必须显式写 ': 类型'（缺 ':' 报错）
 * - 字段以分号分隔
 * - 字段列表可为空（{} 合法——空 cunion 与空 struct 同构，size=1 占位）
 */
ast_node_t *parse_cunion_def(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "cunion")) return NULL;
    advance(p);
    skip_trivia(p);

    /* cunion 名：标识符 */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected cunion name after 'cunion'");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* 字段（member）列表：{ ... } */
    if (!expect_symbol(p, "{")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '{' to start cunion field list");
    }
    skip_trivia(p);

    ast_node_t *fields = NULL, *fields_last = NULL;

    for (;;) {
        /* '}' 结束字段列表（含尾随分号后直接 '}' 的情况） */
        if (check_symbol(p, "}")) break;

        /* 字段（member）名：标识符 */
        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected field name in cunion definition");
        }
        uint32_t fb = p->pos;
        strslice_t fname = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);

        /* 字段类型：: expr（必须） */
        if (!check_symbol(p, ":")) {
            return ast_error_new(p->diag, p->tokens, p->arena, fb, p->pos,
                                 "expected ': type' after cunion field name");
        }
        advance(p);
        skip_trivia(p);

        ast_node_t *ftype = parse_expr(p);
        if (!ftype || ftype->kind == AST_ERROR) {
            if (!ftype) {
                return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos,
                                     "expected field type expression after ':'");
            }
            return ftype;
        }
        skip_trivia(p);

        ast_node_t *fn = ast_struct_field_new(p->arena, fb, p->pos);
        if (!fn) return NULL;
        ((ast_struct_field_t *)fn)->name = fname;
        ((ast_struct_field_t *)fn)->type = ftype;
        ast_append(&fields, &fields_last, NULL, fn);

        /* 分号分隔，} 结束 */
        if (check_symbol(p, ";")) {
            advance(p);
            skip_trivia(p);
            continue;
        }
        break;
    }

    if (!expect_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '}' to end cunion field list");
    }

    ast_node_t *node = ast_cunion_def_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_cunion_def_t *)node)->name         = name;
    ((ast_cunion_def_t *)node)->fields       = fields;
    ((ast_cunion_def_t *)node)->fields_last  = fields_last;
    return node;
}
