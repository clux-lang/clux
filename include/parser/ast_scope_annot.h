#ifndef _H_CLUX_PARSER_AST_SCOPE_ANNOT_
#define _H_CLUX_PARSER_AST_SCOPE_ANNOT_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

/* 作用域标注（m3-design §5）：'<a,b,c> type —— 前导在 ':' 之后，
 * 包裹被标注类型。names 为标注集合（arena 数组，'*' = global 记
 * STRSLICE_LIT("*")），count 为名字个数。标注不是类型的一部分，
 * 是绑定在 value 上的位置信息（Step A 静默：只解析存储，不检查）。 */
typedef struct {
    ast_node_t  base;
    strslice_t *names;      /* 标注集合（arena 数组，含 "*" 记 global） */
    size_t      count;      /* 名字个数 */
    ast_node_t *sub;        /* 被标注的类型表达式 */
} ast_scope_annot_t;

static inline ast_node_t *ast_scope_annot_new(arena_t *arena,
                                              uint32_t tok_begin,
                                              uint32_t tok_end) {
    ast_scope_annot_t *n = (ast_scope_annot_t *)arena_calloc(
        arena, 1, sizeof(ast_scope_annot_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_SCOPE_ANNOT;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_SCOPE_ANNOT_ */
