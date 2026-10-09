#ifndef _H_CLUX_PARSER_AST_MAKE_
#define _H_CLUX_PARSER_AST_MAKE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/* make 编译期宏函数（m4-design §2）：make(T, N, init...) → fatal []T。
 * T 是元素类型表达式（编译期替换，不作为运行期参数），N 是长度表达式
 * （可运行期值），init... 是初始值列表（支持 <v,M> fill 值包，同数组
 * 构造语法）。clux 无默认零值，必须给出所有元素的初始值。
 *
 * sema 层将 make 替换为：分配 len*elem_size 堆块 + 逐元素布值 → fatal []T。
 * 返回 fatal []T（将亡值，须被 own 接管）。 */
typedef struct {
    ast_node_t  base;
    ast_node_t *elem_type;  /* 元素类型表达式 T */
    ast_node_t *length;     /* 长度表达式 N */
    ast_node_t *inits;      /* 初始值兄弟链（init...，可含 AST_FILL） */
    ast_node_t *inits_last;
} ast_make_t;

static inline ast_node_t *ast_make_new(arena_t *arena,
                                       uint32_t tok_begin,
                                       uint32_t tok_end) {
    ast_make_t *n = (ast_make_t *)arena_calloc(
        arena, 1, sizeof(ast_make_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_MAKE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_MAKE_ */
