#include "parser/ast_switch.h"
#include "parser/ast_block.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"

/* switch(cond) { (pat,..)->{..} default->{..} }
   - 条件带括号，可为运行时表达式
   - 分支 (模式列表)->{块}：逗号分隔 = || 链（惰性求值）
   - default 兜底（最多一个）；无 fallthrough
   错误恢复：任何位置失败返回 AST_ERROR（ast_error_new 已记录诊断）。 */
ast_node_t *parse_switch(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "switch")) return NULL;
    advance(p);
    skip_trivia(p);

    /* 条件必须用 () 包裹 */
    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' after 'switch'");
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
                             "expected ')' after switch condition");
    }
    skip_trivia(p);

    if (!expect_symbol(p, "{")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '{' after switch condition");
    }

    ast_node_t *node = ast_switch_new(p->arena, tb, p->pos);
    ast_switch_t *sw = (ast_switch_t *)node;
    ast_node_t *cases = NULL, *cases_last = NULL;
    ast_node_t *default_body = NULL;

    skip_trivia(p);
    while (!check_symbol(p, "}")) {
        skip_trivia(p);
        if (check_keyword(p, "default")) {
            if (default_body) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "duplicate 'default' in switch");
            }
            advance(p);
            skip_trivia(p);
            if (!expect_symbol(p, "->")) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected '->' after 'default'");
            }
            skip_trivia(p);
            default_body = parse_block(p);
            if (!default_body || default_body->kind == AST_ERROR) {
                if (!default_body) {
                    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                         "expected '{' after 'default ->'");
                }
                return default_body;
            }
        } else if (check_symbol(p, "(")) {
            uint32_t case_tb = p->pos;
            advance(p);
            skip_trivia(p);

            ast_node_t *cs = ast_switch_case_new(p->arena, case_tb, p->pos);
            ast_switch_case_t *sc = (ast_switch_case_t *)cs;

            /* 模式列表：expr (, expr)*（逗号分隔 = || 链） */
            ast_node_t *patterns = NULL, *plast = NULL;
            if (check_symbol(p, ")")) {
                return ast_error_new(p->diag, p->tokens, p->arena, case_tb,
                                     p->pos, "expected pattern after '(' in switch");
            }
            while (true) {
                skip_trivia(p);
                ast_node_t *pat = parse_expr(p);
                if (!pat) {
                    return ast_error_new(p->diag, p->tokens, p->arena, case_tb,
                                         p->pos, "expected pattern after '(' in switch");
                }
                if (pat->kind == AST_ERROR) return pat;
                ast_append(&patterns, &plast, NULL, pat);
                skip_trivia(p);
                if (check_symbol(p, ",")) {
                    advance(p);
                    continue;
                }
                break;
            }
            if (!expect_symbol(p, ")")) {
                return ast_error_new(p->diag, p->tokens, p->arena, case_tb,
                                     p->pos, "expected ')' after switch pattern list");
            }
            skip_trivia(p);
            if (!expect_symbol(p, "->")) {
                return ast_error_new(p->diag, p->tokens, p->arena, case_tb,
                                     p->pos, "expected '->' after switch pattern list");
            }
            skip_trivia(p);

            ast_node_t *body = parse_block(p);
            if (!body || body->kind == AST_ERROR) {
                if (!body) {
                    return ast_error_new(p->diag, p->tokens, p->arena, case_tb,
                                         p->pos, "expected '{' after switch case");
                }
                return body;
            }

            sc->patterns = patterns;
            sc->body     = body;
            cs->tok_end  = p->pos;
            ast_append(&cases, &cases_last, NULL, cs);
        } else {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '(' pattern list or 'default' in switch body");
        }
        skip_trivia(p);
    }

    if (!expect_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '}' to close switch");
    }

    sw->cond          = cond;
    sw->cases         = cases;
    sw->default_body  = default_body;
    node->tok_end     = p->pos;
    return node;
}
