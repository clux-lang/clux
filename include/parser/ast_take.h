#ifndef _H_CLUX_PARSER_AST_TAKE_
#define _H_CLUX_PARSER_AST_TAKE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "core/arena.h"

/**
 * AST_TAKE：**把值移出变量**的直接形态（sema 生成，语义 = `move(x)`）。
 *
 * 用于扁平化 clone 的切片路径：sema 先在当前语句的 prelude 里定义临时变量
 * 并填好深拷贝结果，表达式结果 = `AST_TAKE(AST_IDENT(tmp))`——把临时变量的
 * 值移出（compiler 发 `MOVE`），临时变量在 sema 侧已标 `moved`，块退出时
 * 不再生成 FREE。
 *
 * 为什么不复用 AST_MOVE：`move` 的 op 是 `token_t*`，sema 无法合成
 * （与 AST_CLONE 同一原因）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *operand;   /* 被移出的值表达式（通常是生成的临时变量引用） */
} ast_take_t;

static inline ast_node_t *ast_take_new(arena_t *arena, uint32_t tok_begin,
                                       uint32_t tok_end) {
    ast_take_t *n = (ast_take_t *)arena_calloc(
        arena, 1, sizeof(ast_take_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_TAKE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    n->base.is_generated = true;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_TAKE_ */
