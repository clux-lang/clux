#ifndef _H_CLUX_PARSER_AST_FREE_
#define _H_CLUX_PARSER_AST_FREE_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "core/arena.h"

/**
 * AST_FREE：sema 为 own 变量在作用域退出点生成的显式释放语句。
 *
 * 不出现在用户源码中——由 sema desugar_defers 阶段自动生成，插入到
 * 块尾 / return / break / continue 退出点（与 defer calls 同机制）。
 * compiler 识别 AST_FREE 发出 BCODE_FREE <name> 指令。
 *
 * name 是 arena 持久化的 NUL 终止字符串（变量名）。
 */
typedef struct {
    ast_node_t base;
    strslice_t name;  /* 要释放的变量名（arena 持久化） */
} ast_free_t;

static inline ast_node_t *ast_free_new(arena_t *arena, strslice_t name) {
    ast_free_t *n = (ast_free_t *)arena_calloc(
        arena, 1, sizeof(ast_free_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FREE;
    n->base.tok_begin = 0;  /* 编译器生成节点：tok_begin=0 标记 */
    n->base.tok_end   = 0;
    n->name           = name;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_FREE_ */
