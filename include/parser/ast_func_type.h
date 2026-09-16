#ifndef _H_CLUX_PARSER_AST_FUNC_TYPE_
#define _H_CLUX_PARSER_AST_FUNC_TYPE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * 函数签名类型表达式节点：func ( param_type, ... ) [-> return_type]
 *   - params：参数类型表达式兄弟链（head/tail）；可为空（func()）
 *   - return_type：返回类型表达式；NULL = void（缺省）
 * 函数类型是类型构造（M2 §5：type F = func(i32, i32)->i32;），
 * 与 AST_ARRAY 同属"类型即表达式"体系。参数/返回类型可为任意类型
 * 表达式（含嵌套 func(...)->ret、数组、const/volatile）。
 * 由 parse_primary 的 'func' 关键字分支构造。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *params;        /* 参数类型表达式兄弟链（head） */
    ast_node_t *params_last;   /* 参数类型表达式兄弟链（tail） */
    ast_node_t *return_type;   /* 返回类型表达式；NULL = void */
} ast_func_type_t;

static inline ast_node_t *ast_func_type_new(arena_t *arena,
                                            uint32_t tok_begin, uint32_t tok_end) {
    ast_func_type_t *n = (ast_func_type_t *)arena_calloc(
        arena, 1, sizeof(ast_func_type_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FUNC_TYPE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_FUNC_TYPE_ */
