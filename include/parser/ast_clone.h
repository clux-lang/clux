#ifndef _H_CLUX_PARSER_AST_CLONE_
#define _H_CLUX_PARSER_AST_CLONE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "core/arena.h"

/**
 * AST_CLONE：**叶子深拷贝**的直接形态（sema 生成）。
 *
 * 语义 = 源码 `clone(x)` 的叶子情形：申请新块 + 拷贝字节（compiler 发
 * `CLONE` 指令）。只用于**不含 own 槽位**的类型（标量 / 无 own 字段的聚合 /
 * cunion / 指向无 own 槽位内容的指针），因此运行期不需要按类型递归——
 * 复合类型的逐槽位展开由 sema 生成 AST（构造 / NEW / 循环），见
 * `sema_gen_clone_expr`（expr.c）。
 *
 * 为什么不复用 AST_MOVE：`clone` / `move` 的 op 是 `token_t*`，sema 无法合成
 * 词法 token（与 `is` / `!= nil` 同一问题，见 AST_IS_TAG / AST_OPT_IS_SOME）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *operand;   /* 被 clone 的值表达式 */
} ast_clone_t;

static inline ast_node_t *ast_clone_new(arena_t *arena, uint32_t tok_begin,
                                        uint32_t tok_end) {
    ast_clone_t *n = (ast_clone_t *)arena_calloc(
        arena, 1, sizeof(ast_clone_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_CLONE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    n->base.is_generated = true;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_CLONE_ */
