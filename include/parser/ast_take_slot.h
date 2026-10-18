#ifndef _H_CLUX_PARSER_AST_TAKE_SLOT_
#define _H_CLUX_PARSER_AST_TAKE_SLOT_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "core/arena.h"

/**
 * AST_TAKE_SLOT：**把值从内存槽位取出并把槽位置零**（sema 生成，语义 =
 * "真正移出"）。operand 求值得到**指向槽位的借用值**（`c.p` / `s.c.p` /
 * `p.c.p` 这类左值访问），compiler 发 `TAKE_SLOT`：
 *   - 拷贝槽位里的值 → 压回一个独立的新值；
 *   - `memset(槽位, 0, sizeof(T))` —— 源槽位被清空。
 *
 * 用途：**cunion 成员 move**（cunion 是不安全类型：无 tag、不参与所有权运算）。
 * 槽位置零后：`_ = move(c.p)` 是真正的转移（重复 move 变 no-op，不会二次
 * 释放）；`var q = move(c.p)` 之后 cunion 里不再残留悬垂指针。
 *
 * 为什么不复用 AST_MOVE/AST_TAKE：`move` 的 op 是 `token_t*`，sema 无法合成。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *operand;   /* 指向槽位的左值访问表达式 */
} ast_take_slot_t;

static inline ast_node_t *ast_take_slot_new(arena_t *arena, uint32_t tok_begin,
                                            uint32_t tok_end) {
    ast_take_slot_t *n = (ast_take_slot_t *)arena_calloc(
        arena, 1, sizeof(ast_take_slot_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_TAKE_SLOT;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    n->base.is_generated = true;
    return (ast_node_t *)n;
}

#ifdef __cplusplus
}
#endif
#endif
