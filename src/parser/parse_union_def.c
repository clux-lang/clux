#include "parser/ast_union_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_error.h"

/* 解析 tag union 定义 union Name { Tag: {field: type; ...}; Empty; ... }
 *
 * 语法：union 名字 { 标识符 : { 字段 ; ... } ; ... } | { 标识符 ; ... }
 * - member 名 = tag 名；带 payload 的 member 写 ': {fields}'，payload 字段
 *   与 struct 字段同构（AST_STRUCT_FIELD，分号分隔，字段类型表达式）
 * - 无 payload 的纯 tag member 直接写 'Empty;'
 * - member 列表不能为空（{} 报错）
 */
ast_node_t *parse_union_def(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "union")) return NULL;
    advance(p);
    skip_trivia(p);

    /* union 名：标识符 */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected union name after 'union'");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* member 列表：{ ... } */
    if (!expect_symbol(p, "{")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '{' to start union member list");
    }
    skip_trivia(p);

    ast_node_t *members = NULL, *members_last = NULL;

    /* 空 member 列表非法：{} */
    if (check_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "union member list must not be empty");
    }

    for (;;) {
        /* '}' 结束 member 列表（含尾随分号后直接 '}' 的情况） */
        if (check_symbol(p, "}")) break;

        /* member 名：标识符 */
        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected member name in union definition");
        }
        uint32_t mb = p->pos;
        strslice_t mname = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);

        ast_node_t *fields = NULL, *fields_last = NULL;

        if (check_symbol(p, ":")) {
            /* payload 字段表：: { field: type; ... } */
            advance(p);
            skip_trivia(p);
            if (!expect_symbol(p, "{")) {
                return ast_error_new(p->diag, p->tokens, p->arena, mb, p->pos,
                                     "expected '{' to start union member payload");
            }
            skip_trivia(p);

            for (;;) {
                if (check_symbol(p, "}")) break;

                if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
                    return ast_error_new(p->diag, p->tokens, p->arena, mb, p->pos,
                                         "expected field name in union member payload");
                }
                uint32_t fb = p->pos;
                strslice_t fname = token_strslice(cur_token(p));
                advance(p);
                skip_trivia(p);

                if (!check_symbol(p, ":")) {
                    return ast_error_new(p->diag, p->tokens, p->arena, fb, p->pos,
                                         "expected ': type' after union member field name");
                }
                advance(p);
                skip_trivia(p);

                ast_node_t *ftype = parse_expr(p);
                if (!ftype || ftype->kind == AST_ERROR) {
                    if (!ftype) {
                        return ast_error_new(p->diag, p->tokens, p->arena, p->pos,
                                             p->pos,
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

                /* payload 字段以逗号分隔（{w: f32, h: f32}）；分号也可
                   （{a: i32; b: i32}），'}' 结束。 */
                if (check_symbol(p, ",") || check_symbol(p, ";")) {
                    advance(p);
                    skip_trivia(p);
                    continue;
                }
                break;
            }

            if (!expect_symbol(p, "}")) {
                return ast_error_new(p->diag, p->tokens, p->arena, mb, p->pos,
                                     "expected '}' to end union member payload");
            }
            skip_trivia(p);
        }

        ast_node_t *mn = ast_union_member_new(p->arena, mb, p->pos);
        if (!mn) return NULL;
        ((ast_union_member_t *)mn)->name        = mname;
        ((ast_union_member_t *)mn)->fields       = fields;
        ((ast_union_member_t *)mn)->fields_last  = fields_last;
        ast_append(&members, &members_last, NULL, mn);

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
                             "expected '}' to end union member list");
    }

    ast_node_t *node = ast_union_def_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_union_def_t *)node)->name         = name;
    ((ast_union_def_t *)node)->members       = members;
    ((ast_union_def_t *)node)->members_last  = members_last;
    return node;
}
