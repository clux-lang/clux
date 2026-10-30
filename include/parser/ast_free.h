#ifndef _H_CLUX_PARSER_AST_FREE_
#define _H_CLUX_PARSER_AST_FREE_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "core/arena.h"

/**
 * AST_FREE：**释放表达式所指的堆块**（`free(expr)`；作用域退出时释放闭包
 * env 也走同一节点）。
 *
 * C 后端对每个操作数生成一次 `clux_free(...)`（切片先取 data），运行期不做
 * 类型递归。
 *
 * `operand` 是操作数表达式**兄弟链**的链首，用于组合式释放（一次释放多个
 * 槽位）。不出现在用户源码中——由 sema 在作用域退出点（块尾 / return /
 * break / continue）为闭包变量生成；指针/切片的显式 `free(p)` 内建也产本节点。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *operand;  /* 操作数表达式兄弟链链首（释放它所指的块） */
    /* sema 可填：显式释放表达式的类型（切片→取 data，指针→直接）。
       未填时 C 后端回退到 infer_expr_type。 */
    const struct type_t *type;
} ast_free_t;

static inline ast_node_t *ast_free_new(arena_t *arena, ast_node_t *operand) {
    ast_free_t *n = (ast_free_t *)arena_calloc(
        arena, 1, sizeof(ast_free_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FREE;
    n->base.tok_begin = 0;  /* 编译器生成节点：tok_begin=0 标记 */
    n->base.tok_end   = 0;
    n->base.is_generated = true;  /* Pass 3b 不回走（见 stmt.c walk_stmt） */
    n->operand        = operand;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_FREE_ */
