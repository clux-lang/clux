#include "parser/parser.h"
#include "parser/ast_program.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_union_def.h"
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
    p->recover_partial = false;   /* 默认关闭：不影响 driver/测试既有行为 */
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
        } else if (check_keyword(p, "type")) {
            /* 全局类型定义 type name = <type-expr>; 挂 funcs 链：
               sema pass1b 求值折叠（AST_TYPE_REF）后保留——进入字节码，
               运行时 LOAD_TYPE/PUSH; PUSH_UNDEFINED; DEFINE 绑定 type value。 */
            func = parse_type_def(p);
        } else if (check_keyword(p, "enum")) {
            /* 顶层枚举定义 enum Name:Underlying { Var = val, ... } 挂
               funcs 链：sema pass1b 折叠 variant 值 + 登记类型，进入字节码，
               运行时 hoist 构造 enum 类型 + 绑定名字。 */
            func = parse_enum_def(p);
        } else if (check_keyword(p, "struct")) {
            /* 顶层结构体定义 struct Name { field: type; ... } 挂 funcs 链：
               sema pass1b 折叠字段类型 + 登记类型，进入字节码，运行时 hoist
               构造 struct 类型 + 绑定名字。 */
            func = parse_struct_def(p);
        } else if (check_keyword(p, "union")) {
            /* 顶层 tag union 定义 union Name { Tag: {fields}; ... } 挂
               funcs 链：sema pass1b 折叠字段类型 + 登记类型（tag 整数前缀
               + member 联合体布局），进入字节码，运行时 hoist 构造 union
               类型 + 绑定名字。 */
            func = parse_union_def(p);
        } else if (check_keyword(p, "var")) {
            /* 全局变量定义 var name[:type] = init; 挂 funcs 链（非 comptime）。
               sema pass_globals 折叠 init 为字面量/函数引用后保留——进入
               字节码，运行时 DEFINE 到模块 root_scope（与全局函数绑定同层，
               函数体经 root_scope 查找可见）。区别于 comptime var（编译期
               常量，pass_globals 摘除不进运行时）。 */
            func = parse_var_def(p);
        } else if (check_symbol(p, ";")) {
            /* 顶层空语句：单独分号（;）——无操作占位，跳过 */
            advance(p);
            skip_trivia(p);
            continue;
        } else {
            func = parse_func_def(p);
        }
        if (!func) {
            /* 语法错误。recover_partial（formatter 用）时保留错误前的 AST：
               已解析的顶层节点仍挂到 PROGRAM，tok_end 停在错误 token 处。 */
            if (p->recover_partial) {
                p->has_error = true;
                break;
            }
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected function definition at top level");
        }
        if (func->kind == AST_ERROR) {
            if (p->recover_partial) {
                p->has_error = true;
                break;
            }
            return func;
        }

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
