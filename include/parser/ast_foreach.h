#ifndef _H_CLUX_PARSER_AST_FOREACH_
#define _H_CLUX_PARSER_AST_FOREACH_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "core/strslice.h"
#include "parser/parser.h"
#include <stdint.h>

/**
 * AST_FOREACH：**for-each 语法糖的输入形态**（2026-10）。
 *
 *   foreach (a of b) { … }                    // a 为**已定义**变量：每轮 a = b[i]
 *   foreach (var a of b) { … }                // 新声明，类型由元素类型推断
 *   foreach (var a: ref *i32 of b) { … }      // 新声明 + 显式类型（own 元素的
 *                                             //   容器用 `own [] own *i32` 这类，
 *                                             //   元素以 ref *T 借出）
 *
 * 本节点只存在于 parser → **前脱糖** 之间：`desugar_program` 把它展开成普通的
 * 索引 `for` 循环（见 include/desugar/desugar.h），sema / compiler / CTFE
 * 完全看不到 AST_FOREACH。
 *
 * 展开形状（`b` 是裸标识符时省掉临时变量；步进放在 for 的 update 段，于是
 * `continue` 不会跳过步进）：
 *
 *   {
 *     var __arr_N = b;                    // 仅当 b 不是裸标识符
 *     for (var __idx_N: usize = 0usize;
 *          __idx_N < len(__arr_N);
 *          __idx_N = __idx_N + 1usize) {
 *         var a [: T] = __arr_N[__idx_N];   // 或（无 `var`）：a = __arr_N[__idx_N];
 *         <body>
 *     }
 *   }
 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;       /* 迭代变量名 */
    uint32_t    name_tok;   /* 迭代变量名 token 下标（展开后的元素绑定语句
                               用它做诊断定位；0 = 未知） */
    ast_node_t *type_expr;  /* 可选 `: T`（NULL = 推断） */
    ast_node_t *iterable;   /* `of` 之后的表达式 */
    ast_node_t *body;       /* AST_BLOCK */
    bool        is_decl;    /* true = `var a`（新声明）；false = 已定义变量 */
} ast_foreach_t;

static inline ast_node_t *ast_foreach_new(arena_t *arena, uint32_t tok_begin,
                                          uint32_t tok_end) {
    ast_foreach_t *n = (ast_foreach_t *)arena_calloc(
        arena, 1, sizeof(ast_foreach_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FOREACH;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 `foreach ( [var] name [: T] of expr ) { body }`。 */
ast_node_t *parse_foreach(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
