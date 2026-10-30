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

    /* `_ = expr` **显式销毁/丢弃**的语义分类（sema 写、compiler 读；
       sema 的 `shadow_assign` `_` 分支）：
         0 = 普通赋值（非 discard）
         1 = 丢弃**借用/无资源**值：编译 expr 后 `POP`（**不得 DISPOSE**——
             借用不拥有堆块，释放会破坏借用源；实测 0xC0000005）
         2 = 丢弃**自有资源**值：sema 已把销毁序列生成为语句 prelude，
             语句本身不发任何指令
         3 = 回退：运行期 `DISPOSE`（sema 无法生成 prelude 时） */
    uint8_t        discard_mode;

    /* sema 错误处理语法糖：真正赋值在 prelude 的 if 分支里完成，本语句
       只作为占位（compiler 跳过）。 */
    bool           sugar_noop;
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
