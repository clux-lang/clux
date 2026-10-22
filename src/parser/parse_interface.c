#include "parser/ast_interface_def.h"

#include "parser/ast_error.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_struct_embed.h"
#include "parser/ast_var_def.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"

#include <string.h>

/* ===========================================================================
 * M7 §31：interface 声明解析
 *
 *     interface IWriter[T] {
 *         var fd: i32;
 *         func write(self: *T, data: ref []u8);
 *         ...Base;                 // 成员复制（P6c）
 *     }
 *
 * 成员只允许：无初始化 var（显式类型）、无函数体 func 签名、`...Base;`。
 * ========================================================================= */

static ast_node_t *parse_iface_func(parser_t *p, uint32_t tb) {
    /* 已消费 'func' */
    skip_trivia(p);
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected method name in interface");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);
    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after method name in interface");
    }
    skip_trivia(p);

    ast_node_t *params = NULL, *params_last = NULL;
    if (!check_symbol(p, ")")) {
        for (;;) {
            ast_node_t *param = parse_func_param_decl(p);
            if (!param || param->kind == AST_ERROR) {
                if (!param)
                    return ast_error_new(p->diag, p->tokens, p->arena, tb,
                                         p->pos,
                                         "expected parameter in interface method");
                return param;
            }
            ast_append(&params, &params_last, NULL, param);
            skip_trivia(p);
            if (!check_symbol(p, ",")) break;
            advance(p);
            skip_trivia(p);
        }
    }
    if (!expect_symbol(p, ")")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ')' after interface method parameters");
    }
    skip_trivia(p);
    ast_node_t *ret = NULL;
    if (check_symbol(p, ":")) {
        advance(p);
        skip_trivia(p);
        ret = parse_scope_annotated_type(p);
        if (!ret || ret->kind == AST_ERROR) {
            if (!ret)
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected return type in interface method");
            return ret;
        }
        skip_trivia(p);
    }
    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "interface method declaration must end with ';' "
                             "(no body allowed)");
    }

    ast_node_t *fn = ast_func_def_new(p->arena, tb, p->pos);
    if (!fn) return NULL;
    ast_func_def_t *fd = (ast_func_def_t *)fn;
    fd->name = name;
    fd->params = params;
    fd->params_last = params_last;
    fd->return_expr = ret;
    fd->body = NULL;
    return fn;
}

static ast_node_t *parse_iface_var(parser_t *p, uint32_t tb) {
    /* 已消费 'var' */
    skip_trivia(p);
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected attribute name in interface");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);
    if (!expect_symbol(p, ":")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "interface attribute requires an explicit type");
    }
    skip_trivia(p);
    ast_node_t *type = parse_scope_annotated_type(p);
    if (!type || type->kind == AST_ERROR) {
        if (!type)
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected attribute type in interface");
        return type;
    }
    skip_trivia(p);
    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "interface attribute must end with ';'");
    }
    ast_node_t *vd = ast_var_def_new(p->arena, tb, p->pos);
    if (!vd) return NULL;
    ((ast_var_def_t *)vd)->name = name;
    ((ast_var_def_t *)vd)->type_expr = type;
    ((ast_var_def_t *)vd)->init = NULL;
    return vd;
}

ast_node_t *parse_interface_def(parser_t *p) {
    uint32_t tb = p->pos;
    if (!check_keyword(p, "interface")) return NULL;
    advance(p);
    skip_trivia(p);

    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected interface name after 'interface'");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    ast_generic_params_t generic = {0};
    if (check_symbol(p, "[")) {
        ast_node_t *gerr = parse_generic_params(p, &generic);
        if (gerr) return gerr;
    }

    if (!expect_symbol(p, "{")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '{' to start interface body");
    }
    skip_trivia(p);

    ast_node_t *members = NULL, *members_last = NULL;
    ast_node_t *embeds = NULL, *embeds_last = NULL;

    for (;;) {
        skip_trivia(p);
        if (check_symbol(p, "}")) break;

        if (check_symbol(p, "...")) {
            uint32_t eb = p->pos;
            advance(p);
            skip_trivia(p);
            /* M7 §31.6：`...Base;` 或带实参的 `...IWriter[T];`。 */
            ast_node_t *et = parse_type_with_index(p);
            if (!et || et->kind == AST_ERROR) {
                if (!et)
                    return ast_error_new(p->diag, p->tokens, p->arena, eb,
                                         p->pos,
                                         "expected interface type after '...'");
                return et;
            }
            skip_trivia(p);
            if (!expect_symbol(p, ";")) {
                return ast_error_new(p->diag, p->tokens, p->arena, eb, p->pos,
                                     "expected ';' after '...Interface' in "
                                     "interface");
            }
            ast_node_t *en = ast_struct_embed_new(p->arena, eb, p->pos);
            if (!en) return NULL;
            ast_struct_embed_t *emb = (ast_struct_embed_t *)en;
            emb->type_expr = et;
            if (et->kind == AST_IDENT)
                emb->name = ((ast_ident_t *)et)->name;
            else if (et->kind == AST_INDEX && ((ast_index_t *)et)->object &&
                     ((ast_index_t *)et)->object->kind == AST_IDENT)
                emb->name = ((ast_ident_t *)((ast_index_t *)et)->object)->name;
            ast_append(&embeds, &embeds_last, NULL, en);
            continue;
        }

        if (check_keyword(p, "comptime")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "interface members cannot be 'comptime'");
        }

        if (check_keyword(p, "var")) {
            advance(p);
            ast_node_t *m = parse_iface_var(p, tb);
            if (!m || m->kind == AST_ERROR) return m;
            ast_append(&members, &members_last, NULL, m);
            continue;
        }

        if (check_keyword(p, "func")) {
            uint32_t fb = p->pos;
            advance(p);
            ast_node_t *m = parse_iface_func(p, fb);
            if (!m || m->kind == AST_ERROR) return m;
            m->tok_begin = fb;
            ast_append(&members, &members_last, NULL, m);
            continue;
        }

        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "interface body only allows 'var name: type;', "
                             "'func name(params): type;' or '...Base;'");
    }

    if (!expect_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '}' to end interface body");
    }

    ast_node_t *node = ast_interface_def_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ast_interface_def_t *id = (ast_interface_def_t *)node;
    id->name = name;
    id->generic = generic;
    id->members = members;
    id->members_last = members_last;
    id->embeds = embeds;
    id->embeds_last = embeds_last;
    return node;
}
