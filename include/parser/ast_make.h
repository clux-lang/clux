#ifndef _H_CLUX_PARSER_AST_MAKE_
#define _H_CLUX_PARSER_AST_MAKE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/* make 切片视图构造：make(pointer, len) → []T。
 *
 * pointer 是 *T 表达式（通常来自 libc::malloc 或数组元素取址），len 是
 * usize 长度。**不分配任何内存**——只是把已有的指针 + 长度包成切片胖指针；
 * 缓冲区的生命周期完全由调用方负责（与 C 一致）。 */
typedef struct {
    ast_node_t  base;
    ast_node_t *pointer;    /* *T 指针表达式 */
    ast_node_t *length;     /* 长度表达式 */
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
