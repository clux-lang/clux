#ifndef _H_CLUX_PARSER_AST_ADDR_
#define _H_CLUX_PARSER_AST_ADDR_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/* 后置取地址（m3-design §8.2）：x.& —— 由值得指针。
 * 与字段访问 '.' 同一位置族（parse_postfix 的 '.' 分派）。 */
typedef struct {
    ast_node_t  base;
    ast_node_t *operand;    /* . 左边的表达式 */
} ast_addr_t;

static inline ast_node_t *ast_addr_new(arena_t *arena,
                                       uint32_t tok_begin, uint32_t tok_end) {
    ast_addr_t *n = (ast_addr_t *)arena_calloc(
        arena, 1, sizeof(ast_addr_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_ADDR;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_ADDR_ */
