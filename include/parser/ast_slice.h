#ifndef _H_CLUX_PARSER_AST_SLICE_
#define _H_CLUX_PARSER_AST_SLICE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/* 切片表达式（m4-design §4）：expr[low:high]。
 * 适用于数组/切片/str，恒产 ref []T（借用胖指针）。
 * low/high 均可省略（arr[:] 全范围；arr[:n] 前缀；arr[n:] 后缀），
 * 省略时对应字段为 NULL（sema 层解释为 0 / len）。
 * arr[:] 是显式数组转切片的唯一路径（[N]T → ref []T 不自动）。 */
typedef struct {
    ast_node_t  base;
    ast_node_t *object;  /* [ 左边的表达式（数组 / 切片 / str） */
    ast_node_t *low;     /* 下界表达式（可 NULL = 0） */
    ast_node_t *high;    /* 上界表达式（可 NULL = len） */
} ast_slice_t;

static inline ast_node_t *ast_slice_new(arena_t *arena,
                                        uint32_t tok_begin,
                                        uint32_t tok_end) {
    ast_slice_t *n = (ast_slice_t *)arena_calloc(
        arena, 1, sizeof(ast_slice_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_SLICE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_SLICE_ */
