#ifndef _H_CLUX_PARSER_AST_TYPE_REF_
#define _H_CLUX_PARSER_AST_TYPE_REF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

/* 类型引用节点：对 sema 登记具名类型（__type_N）的引用。
 * 由 sema 在类型槽位替换（resolve_type_expr 结果写回）时构造，
 * compiler 遇此节点发 LOAD_TYPE <id>（id 经名字在 sema->types 表查询）。
 * AST 保持平凡可解耦：只携带名字标识，不关联任何 type_t 指针。 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;        /* 具名类型标识，如 "__type_0"（sema arena 生命周期） */
} ast_type_ref_t;

static inline ast_node_t *ast_type_ref_new(arena_t *arena,
                                           uint32_t tok_begin, uint32_t tok_end) {
    ast_type_ref_t *n = (ast_type_ref_t *)arena_calloc(
        arena, 1, sizeof(ast_type_ref_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_TYPE_REF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_TYPE_REF_ */
