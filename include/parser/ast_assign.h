#ifndef _H_CLUX_PARSER_AST_ASSIGN_
#define _H_CLUX_PARSER_AST_ASSIGN_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/lexer.h"

typedef struct {
    ast_node_t     base;
    ast_node_t    *target;      /* 左值表达式子节点（目前仅支持 AST_IDENT） */
    const token_t *op;          /* 赋值运算符 token（= / += / -= / *= / /= / %=） */
    ast_node_t    *value;       /* 右值 */
    /* 赋值点**旧值销毁序列**（sema 生成；语句链，compiler 在「右值求值之后、
       写入之前」逐条编译）。用于 `p = new …` 这类覆盖自有堆块的位置：
       旧块必须先释放，否则再也找不到 → 泄漏。复合赋值不需要（同址运算）。 */
    ast_node_t    *destroy;
} ast_assign_t;

static inline ast_node_t *ast_assign_new(arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end) {
    ast_assign_t *n = (ast_assign_t *)arena_calloc(
        arena, 1, sizeof(ast_assign_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_ASSIGN;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif
