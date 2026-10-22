#ifndef _H_CLUX_PARSER_AST_GENERIC_PARAM_
#define _H_CLUX_PARSER_AST_GENERIC_PARAM_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stddef.h>

/* 泛型参数声明（M7）：
 *
 *     func f[T, U = i32, N: comptime u64, ...Args](...)
 *
 * 每个参数是一个 AST_GENERIC_PARAM 节点，同一声明的参数挂在兄弟链上。
 * 参数形态（设计 m7-design §1/§2/§3/§30.1）：
 *   - 普通类型参数：T
 *   - 带默认值：T = i32（默认值表达式在定义点编译期求值）
 *   - comptime 值参数：N: comptime u64（type_expr 是值类型，可带默认值）
 *   - 参数包：...Args（必须位于列表末尾，每个声明最多一个）
 *   - 结构约束：T extends <pattern>（PATTERN 暂存为表达式，P6 引入专用的
 *     通配符语法；解析器在 P0..P5 阶段对 `extends` 报"尚未实现"）
 *
 * `constraint` / `default_expr` 为 NULL 表示未声明。 */
typedef struct {
    ast_node_t  base;          /* kind = AST_GENERIC_PARAM */
    strslice_t  name;          /* 参数名（类型参数 T / 值参数 N / 包名 Args） */
    bool        is_comptime;   /* true = comptime 值参数（N: comptime u64） */
    bool        is_pack;       /* true = 参数包（...Args） */
    ast_node_t *type_expr;     /* is_comptime 时：编译期值参数的类型表达式 */
    ast_node_t *constraint;    /* extends 右侧 PATTERN（可 NULL，P6 使用） */
    ast_node_t *default_expr;  /* 默认值（可 NULL） */
} ast_generic_param_t;

/* 一个声明上的泛型参数表（内嵌进 func/struct/enum/union/type def 节点）。
 * first/last 是兄弟链端点（O(1) 追加），count 是参数个数。 */
typedef struct {
    ast_node_t *first;
    ast_node_t *last;
    size_t      count;
} ast_generic_params_t;

static inline ast_node_t *ast_generic_param_new(arena_t *arena,
                                                uint32_t tok_begin,
                                                uint32_t tok_end) {
    ast_generic_param_t *n = (ast_generic_param_t *)arena_calloc(
        arena, 1, sizeof(ast_generic_param_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_GENERIC_PARAM;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

static inline void ast_generic_params_add(ast_generic_params_t *list,
                                          ast_node_t *param) {
    if (!list || !param) return;
    param->next = NULL;
    if (!list->first) {
        list->first = param;
        list->last  = param;
    } else {
        list->last->next = param;
        list->last       = param;
    }
    list->count++;
}

/**
 * 解析泛型参数列表（当前 token 必须是 '['）。
 *
 * 成功：消费到匹配的 ']' 之后，填充 *out，返回 NULL。
 * 失败：诊断已记录，返回 AST_ERROR 节点（调用方原样返回）。
 *
 * 语法：
 *     '[' param (',' param)* ']'
 *     param := ['...'] IDENT [':' 'comptime' type_expr] ['extends' pattern]
 *              ['=' expr]
 *
 * 约束（P0 边界）：`extends` 目前直接报"尚未实现"并跳到本参数的 ','/']'，
 * 解析器不对 PATTERN 做部分解析——避免把半截表达式塞进 AST。
 */
ast_node_t *parse_generic_params(parser_t *p, ast_generic_params_t *out);

#ifdef __cplusplus
}
#endif
#endif
