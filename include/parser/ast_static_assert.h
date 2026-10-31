#ifndef _H_CLUX_PARSER_AST_STATIC_ASSERT_
#define _H_CLUX_PARSER_AST_STATIC_ASSERT_

#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/**
 * AST_STATIC_ASSERT：**编译期断言语句**（2026-10）。
 *
 *   static_assert(<cond>, "<msg>");
 *
 * 标准语句：cond 在编译期求值（CTFE），必须是 bool；true → 无操作，false →
 * 编译错误（消息取 msg 的字符串字面量文本）。不生成任何运行期代码。
 *
 * 位于 `comptime if` 未选中分支里的 static_assert 不参与语义检查（随剪枝
 * 一起消失），因此可以用它做平台/泛型相关的条件断言：
 *
 *   func f[T](x: T): void {
 *     comptime if (T == i32) {
 *       static_assert(false, "f: T must not be i32");
 *     }
 *   }
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *cond; /* 布尔表达式（占位；存活时直接报错，不做求值） */
    ast_node_t *msg;  /* 消息表达式（字符串字面量时用于诊断文本） */
} ast_static_assert_t;

static inline ast_node_t *ast_static_assert_new(arena_t *arena,
                                                uint32_t tok_begin,
                                                uint32_t tok_end) {
    ast_static_assert_t *n = (ast_static_assert_t *)arena_calloc(
        arena, 1, sizeof(ast_static_assert_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind = AST_STATIC_ASSERT;
    n->base.tok_begin = tok_begin;
    n->base.tok_end = tok_end;
    return &n->base;
}

#define ast_static_assert_cond(n) (((ast_static_assert_t *)(n))->cond)
#define ast_static_assert_msg(n) (((ast_static_assert_t *)(n))->msg)

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_STATIC_ASSERT_ */
