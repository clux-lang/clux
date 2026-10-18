#ifndef _H_CLUX_PARSER_AST_COMPTIME_IF_
#define _H_CLUX_PARSER_AST_COMPTIME_IF_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * AST_COMPTIME_IF：**编译期条件语句**（2026-10）。
 *
 *   comptime if (const_expr) { ... } else if (...) { ... } else { ... }
 *
 * 语义：**语法树裁剪**，发生在语义层 Pass 3a：
 *   - 条件必须在该点可编译期求值（字面量 / 全局 comptime var / sizeof …
 *     经 CTFE 求值，结果必须是 bool）；
 *   - 存活分支的语句被**拼接**进外层块（大括号是**剪枝范围，不是作用域**）：
 *     `comptime if (f) { var a = 1; } else { var a = 2; } printf(a);` 合法；
 *   - 丢弃分支**只经过解析期语法检查**，不做任何语义检查（不注册符号、
 *     不检查类型、不分配 fid）——这正是它存在的意义（平台相关代码可以
 *     类型完全不同）；
 *   - 分支内**禁止** `comptime var` / `comptime func` 定义（否则"编译期实体
 *     随分支存活"会与拼接语义纠缠）；结构检查覆盖嵌套块，但**不**进入
 *     函数体（嵌套函数体是独立作用域）；
 *   - `comptime if` 不能出现在 `comptime func` 体内（CTFE 解释执行，条件可能
 *     依赖参数）。
 *
 * sema 之后该节点不再出现在语句链中（已被替换为存活分支的语句链）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *cond;        /* 编译期常量表达式（bool） */
    ast_node_t *then_body;   /* AST_BLOCK */
    ast_node_t *else_body;   /* AST_BLOCK / AST_COMPTIME_IF（else-if）或 NULL */
} ast_comptime_if_t;

static inline ast_node_t *ast_comptime_if_new(arena_t *arena,
                                              uint32_t tok_begin,
                                              uint32_t tok_end) {
    ast_comptime_if_t *n = (ast_comptime_if_t *)arena_calloc(
        arena, 1, sizeof(ast_comptime_if_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_COMPTIME_IF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 `comptime if (...)`（含 else-if 链）。 */
ast_node_t *parse_comptime_if(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
