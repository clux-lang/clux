#ifndef _H_CLUX_PARSER_AST_FUNC_REF_
#define _H_CLUX_PARSER_AST_FUNC_REF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

/* 函数引用节点：对函数的引用（函数值），只携带函数 id（fid）。
 *
 * 函数定义 AST 托管在 sema->funcs（sema_func_t::def），fid 由 sema 创建
 * 函数对象时统一分配（写回 ast_func_def_t::fid）。本节点是纯 id 标识：
 *   - 源码名字引用：sema 在 AST_IDENT 求值确认命中函数符号时替换构造
 *     （从符号表 fid 字段取 id，含内建函数 printf=0）
 *   - comptime 折叠产物：sema_ct_lit 折叠函数值常量时构造（从 func_t->id
 *     取 fid）——匿名字面量无名字也可折叠
 * compiler 遇此节点自然发 LOAD_FUNCTION <fid>（与函数是否有 name 无关）；
 * sema/ctfe 需要函数 AST 时经 sema_func_by_id(fid) 从 sema->funcs 查询。 */
typedef struct {
    ast_node_t  base;
    uint32_t    fid;         /* 函数 id（sema 分配；内建函数 = 内建 id） */
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
