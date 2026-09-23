#ifndef _H_CLUX_PARSER_AST_TUPLE_
#define _H_CLUX_PARSER_AST_TUPLE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * 元组类型表达式节点：<T1,T2,...>
 *   - elem_types：元素类型表达式兄弟链（AST_IDENT("i32") / AST_CONST(...) /
 *                 嵌套 AST_ARRAY / 嵌套 AST_TUPLE），逗号分隔
 * 元组类型本身是表达式节点（如 sizeof(<i32,i32>) 天然成立），纳入表达式体系。
 * 由 parse_primary 的 '<' 分支构造（原子位置消歧：比较 '<' 是中缀，构造
 * 字段链 <v,N> 是 fill 值包，三处互不重叠）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *elem_types;     /* 元素类型表达式兄弟链（逗号分隔） */
    ast_node_t *elem_types_last;
} ast_tuple_t;

static inline ast_node_t *ast_tuple_new(arena_t *arena,
                                        uint32_t tok_begin, uint32_t tok_end) {
    ast_tuple_t *n = (ast_tuple_t *)arena_calloc(
        arena, 1, sizeof(ast_tuple_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_TUPLE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_TUPLE_ */
