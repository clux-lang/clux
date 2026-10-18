#include "parser/ast_foreach.h"
#include "parser/ast_block.h"
#include "parser/ast_scope_annot.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

/* 解析 foreach 语句（`foreach` 关键字已在 parse_stmt 分派处消费）：

     foreach (a of b) { … }                 // 已定义变量：每轮 a = b[i]
     foreach (var a of b) { … }             // 新声明（类型由元素推断）
     foreach (var a: ref *i32 of b) { … }   // 新声明 + 显式类型

   只构造 AST_FOREACH（语法糖的输入形态）；真正的展开在前脱糖层
   （src/desugar/desugar.c）用合成 token 完成，sema 看不到本节点。 */
ast_node_t *parse_foreach(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "foreach")) return NULL;
    advance(p);
    skip_trivia(p);

    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'foreach'");
    }
    skip_trivia(p);

    /* 可选 `var`：带 var = 新声明；不带 = 使用已定义变量 */
    bool is_decl = false;
    if (check_keyword(p, "var")) {
        is_decl = true;
        advance(p);
        skip_trivia(p);
    }

    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected iteration variable name in 'foreach'");
    }
    {
        ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p),
                                                     "iteration variable");
        if (rerr) return rerr;
    }
    strslice_t name = token_strslice(cur_token(p));
    uint32_t name_tok = p->pos; /* 展开后的元素绑定语句用它做诊断定位 */
    advance(p);
    skip_trivia(p);

    /* 可选类型标注 `: T`（own 元素的遍历必须写，例如 `: ref *i32`） */
    ast_node_t *type_expr = NULL;
    if (check_symbol(p, ":")) {
        advance(p);
        skip_trivia(p);
        if (!is_decl) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "a type annotation requires 'var' in 'foreach' "
                                 "(write `foreach (var a: T of b)`)");
        }
        type_expr = parse_scope_annotated_type(p);
        if (!type_expr || type_expr->kind == AST_ERROR) {
            if (!type_expr) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected type after ':' in 'foreach'");
            }
            return type_expr;
        }
        skip_trivia(p);
    }

    if (!check_keyword(p, "of")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected 'of' after iteration variable in 'foreach'");
    }
    advance(p);
    skip_trivia(p);

    ast_node_t *iterable = parse_expr(p);
    if (!iterable || iterable->kind == AST_ERROR) {
        if (!iterable) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected expression after 'of' in 'foreach'");
        }
        return iterable;
    }
    skip_trivia(p);

    if (!expect_symbol(p, ")")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ')' after foreach header");
    }
    skip_trivia(p);

    ast_node_t *body = parse_block(p);
    if (!body || body->kind == AST_ERROR) {
        if (!body) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '{' after foreach header");
        }
        return body;
    }
    skip_trivia(p);

    ast_node_t *node = ast_foreach_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_foreach_t *)node)->name      = name;
    ((ast_foreach_t *)node)->name_tok  = name_tok;
    ((ast_foreach_t *)node)->type_expr = type_expr;
    ((ast_foreach_t *)node)->iterable  = iterable;
    ((ast_foreach_t *)node)->body      = body;
    ((ast_foreach_t *)node)->is_decl   = is_decl;
    return node;
}
