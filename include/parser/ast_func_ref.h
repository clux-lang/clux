#ifndef _H_CLUX_PARSER_AST_FUNC_REF_
#define _H_CLUX_PARSER_AST_FUNC_REF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

/* 函数引用节点：对全局函数名的引用（函数值）。
 * 由 sema 在 AST_IDENT 求值确认命中函数符号（sym->type 为签名类型）时
 * 替换构造——sema 是唯一能区分"函数引用 vs 变量引用（含遮蔽）"的位置，
 * 改写后 compiler 遇此节点发 LOAD_FUNCTION <id>（id 经名字在 compiler 的
 * 函数名→fid 映射查询）。AST 保持平凡可解耦：只携带名字标识。 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;        /* 函数名（sema arena 生命周期） */
} ast_func_ref_t;

static inline ast_node_t *ast_func_ref_new(arena_t *arena,
                                           uint32_t tok_begin, uint32_t tok_end) {
    ast_func_ref_t *n = (ast_func_ref_t *)arena_calloc(
        arena, 1, sizeof(ast_func_ref_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FUNC_REF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_FUNC_REF_ */
