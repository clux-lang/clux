#include "parser/parser.h"
#include "parser/ast_program.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"
#include "parser/ast_error.h"
#include "parser/parse_stmt.h"
#include "parser/parse_utils.h"
#include "parser/lexer.h"
#include "parser/ast_node.h"
#include "core/vec.h"
#include "core/panic.h"

/* ---- Internal: class for parser_t ---- */

static class_t g_parser_class = {
    .name = "clux.parser",
    .size = sizeof(char),
    .clone_fn = NULL,
    .move_fn = NULL,
    .dispose_fn = NULL,
};

/* ---- Public API ---- */

parser_t *parser_create(allocator_t *alloc, arena_t *arena, vec_t *tokens) {
    if (!alloc || !arena || !tokens) return NULL;

    parser_t *p = (parser_t *)allocator_new_ex(
        alloc,
        g_parser_class.name,
        /*size=*/1,
        /*move_fn=*/NULL,
        /*clone_fn=*/NULL,
        /*dispose_fn=*/NULL,
        /*count=*/sizeof(parser_t));
    if (!p) panic("parser: out of memory");

    p->alloc     = alloc;
    p->arena     = arena;
    p->tokens    = tokens;
    p->pos       = 0;
    p->has_error = false;
    p->diag      = NULL;
    return p;
}

void parser_destroy(parser_t **pp) {
    if (!pp || !*pp) return;
    parser_t *p = *pp;
    allocator_free(p->alloc, (void **)pp);
}

/* ---- parse_program: 顶层入口 ---- */

/**
 * 检查 token pool 中是否含有词法错误。
 * 如有：输出诊断 → 置 has_error → 返回 true。
 */
static bool has_lexical_errors(parser_t *p) {
    size_t count = vec_len(p->tokens);
    for (size_t i = 0; i < count; i++) {
        const token_t *t = (const token_t *)vec_get(p->tokens, i);
        if (token_get_kind(t) == TOKEN_TYPE_ERROR) {
            const location_t *loc = token_get_location(t);
            const char *msg = token_get_error_message(t);
            fprintf(stderr, "%s:%zu:%zu: error: %s\n",
                    loc ? loc->filename : "<unknown>",
                    loc ? loc->begin.line : 0,
                    loc ? loc->begin.column : 0,
                    msg ? msg : "lexical error");
            p->has_error = true;
            return true;
        }
    }
    return false;
}

ast_node_t *parse_program(parser_t *p) {
    uint32_t tb = p->pos;

    /* parse_program 没有父节点，入口自行跳 trivia */
    skip_trivia(p);

    ast_node_t *funcs      = NULL;
    ast_node_t *funcs_last = NULL;

    while (!at_end(p)) {
        ast_node_t *func = NULL;
        if (check_keyword(p, "comptime")) {
            /* comptime 前缀：comptime func 标记 + 挂链；comptime var（全局
               编译期常量）挂 funcs 链，sema pass_globals 消费后摘除。 */
            uint32_t ctb = p->pos;
            advance(p);
            skip_trivia(p);
            if (check_keyword(p, "func")) {
                func = parse_func_def(p);
                if (func && func->kind != AST_ERROR)
                    ((ast_func_def_t *)func)->is_comptime = true;
            } else if (check_keyword(p, "var")) {
                func = parse_var_def(p);
                if (func && func->kind != AST_ERROR)
                    ((ast_var_def_t *)func)->is_comptime = true;
            } else {
                func = ast_error_new(p->diag, p->tokens, p->arena, ctb, p->pos,
                                     "expected 'func' or 'var' after 'comptime'");
            }
        } else {
            func = parse_func_def(p);
        }
        if (!func) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected function definition at top level");
        }
        if (func->kind == AST_ERROR) return func;

        ast_append(&funcs, &funcs_last, NULL, func);
        skip_trivia(p);
    }

    ast_node_t *node = ast_program_new(p->arena, tb, p->pos);
    ast_program_t *prog = (ast_program_t *)node;
    prog->funcs      = funcs;
    prog->funcs_last = funcs_last;
    return node;
}

ast_node_t *parser_parse(parser_t *p) {
  if (!p) return NULL;

  /* 词法错误检查：有词法错误则不进入解析 */
  if (has_lexical_errors(p)) return NULL;

  return parse_program(p);
}
