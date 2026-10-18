#ifndef _H_CLUX_PARSER_AST_FOR_INDEX_
#define _H_CLUX_PARSER_AST_FOR_INDEX_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "core/arena.h"

/**
 * AST_FOR_INDEX：`for (i = 0; i < len(source); i = i + 1) { body }` 的
 * **直接形态**（sema 生成，用于销毁序列的逐元素循环）。
 *
 * 为什么不拼 AST_FOR + AST_BINARY：循环条件/自增需要 `<` / `+` token，
 * 而 `token_t*` 由 lexer 拥有、sema 无法合成（与 `is` / `!= nil` 同一问题）。
 * 本节点把结构直接交给 compiler：计数器是编译器私有的 `i64` 变量（名字
 * `var`，arena NUL 串，块内唯一），长度由 `LENGTH(source)` 在运行期取。
 *
 * 典型生成（`own []T` / `[N]T` 元素含 own 槽位）：
 *   body 内的语句以 `source[var]` 访问当前元素（AST_INDEX，索引 = AST_IDENT(var)），
 *   例如 `FREE source[i]` 或 `if (source[i] is m) { … }`。
 *
 * 不出现在用户源码中（`is_generated`）。
 */
typedef struct {
    ast_node_t  base;
    strslice_t  var;     /* 计数器名（arena NUL 终止字符串；块内唯一） */
    ast_node_t *source;  /* 容器表达式（取 len；body 里作为元素访问的 base） */
    ast_node_t *body;    /* AST_BLOCK（语句链，引用 var） */
} ast_for_index_t;

static inline ast_node_t *ast_for_index_new(arena_t *arena,
                                            uint32_t tok_begin,
                                            uint32_t tok_end) {
    ast_for_index_t *n = (ast_for_index_t *)arena_calloc(
        arena, 1, sizeof(ast_for_index_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FOR_INDEX;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    n->base.is_generated = true;  /* Pass 3b 不回走（见 stmt.c walk_stmt） */
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_FOR_INDEX_ */
