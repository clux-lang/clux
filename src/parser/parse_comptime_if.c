#include "parser/ast_comptime_if.h"
#include "parser/ast_block.h"
#include "parser/ast_binary.h"
#include "parser/parse_expr.h"
#include "parser/ast_pattern.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

/* comptime if 条件支持 `T extends PATTERN`（含 `?` 通配）：
   先以高于 extends 的 binding 解析 lhs；若是 extends，则 RHS 用 PATTERN
   语法解析，生成 AST_BINARY(extends, lhs, pattern)。CTFE 在求值时识别
   pattern RHS 走 sema_pattern_match。普通条件回退到 parse_expr。 */
static ast_node_t *parse_comptime_cond(parser_t *p) {
    uint32_t save = p->pos;
    ast_node_t *lhs = parse_expr_prec(p, 12);
    if (lhs && lhs->kind != AST_ERROR) {
        skip_trivia(p);
        if (check_keyword(p, "extends")) {
            const token_t *op = cur_token(p);
            advance(p);
            skip_trivia(p);
            ast_node_t *pat = parse_pattern(p);
            if (!pat || pat->kind == AST_ERROR) return pat;
            skip_trivia(p);
            ast_node_t *bin = ast_binary_new(p->arena, lhs->tok_begin, p->pos);
            if (!bin) return NULL;
            ast_binary_t *b = (ast_binary_t *)bin;
            b->op = (token_t *)op;
            b->lhs = lhs;
            b->rhs = pat;
            lhs->next = NULL;
            pat->next = NULL;
            return bin;
        }
    }
    p->pos = save;
    skip_trivia(p);
    return parse_expr(p);
}

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

    ast_node_t *cond = parse_comptime_cond(p);
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
