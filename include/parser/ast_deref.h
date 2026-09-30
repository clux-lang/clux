#ifndef _H_CLUX_PARSER_AST_DEREF_
#define _H_CLUX_PARSER_AST_DEREF_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/* 后置解引用取值（m3-design §8.2）：r.* —— 由指针得值（GET）。
 * 解引用 GET/SET 严格分离：r.* = v 的解引用写（SET）由 AST_ASSIGN
 * 的 target 形态承载（sema/compiler 识别 AST_DEREF target）。 */
typedef struct {
    ast_node_t  base;
    ast_node_t *operand;    /* . 左边的表达式（指针） */
} ast_deref_t;

static inline ast_node_t *ast_deref_new(arena_t *arena,
                                        uint32_t tok_begin, uint32_t tok_end) {
    ast_deref_t *n = (ast_deref_t *)arena_calloc(
        arena, 1, sizeof(ast_deref_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_DEREF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_DEREF_ */
