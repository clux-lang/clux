#include "parser/ast_comptime_if.h"
#include "parser/ast_block.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

/* 解析 `comptime if (...) { ... } [else ...]`（`comptime` 已被
   parse_comptime_stmt 消费）。与 parse_if 同形：条件必须 () 包裹、
   then 必须是块、else 可以是块或嵌套 comptime if（else-if 链）。 */
ast_node_t *parse_comptime_if(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "if")) return NULL;
    advance(p);
    skip_trivia(p);

    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'comptime if'");
    }
    skip_trivia(p);

    ast_node_t *cond = parse_expr(p);
    if (!cond || cond->kind == AST_ERROR) {
        if (!cond) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected condition after '('");
        }
        return cond;
    }
    skip_trivia(p);

    if (!expect_symbol(p, ")")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ')' after comptime if condition");
    }
    skip_trivia(p);

    ast_node_t *then_body = parse_block(p);
    if (!then_body || then_body->kind == AST_ERROR) {
        if (!then_body) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '{' after comptime if condition");
        }
        return then_body;
    }
    uint32_t after_then = p->pos;
    skip_trivia(p);

    ast_node_t *else_body = NULL;
    if (check_keyword(p, "else")) {
        advance(p);
        skip_trivia(p);
        if (check_keyword(p, "comptime")) {
            /* `else comptime if (...) { ... }` */
            advance(p);
            skip_trivia(p);
            else_body = parse_comptime_if(p);
        } else if (check_keyword(p, "if")) {
            /* `else if (...)`：comptime if 的 else 分支本来就是编译期的，
               写法与普通 if 链一致，语义等价于 `else comptime if` */
            else_body = parse_comptime_if(p);
        } else {
            else_body = parse_block(p);
        }
        if (!else_body || else_body->kind == AST_ERROR) {
            if (!else_body) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected block or 'comptime if' after 'else'");
            }
            return else_body;
        }
    } else {
        p->pos = after_then; /* tok_end 覆盖 then_body（fmt 注释挂载） */
    }

    ast_node_t *node = ast_comptime_if_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_comptime_if_t *)node)->cond      = cond;
    ((ast_comptime_if_t *)node)->then_body = then_body;
    ((ast_comptime_if_t *)node)->else_body = else_body;
    return node;
}
