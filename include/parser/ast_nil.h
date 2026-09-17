#ifndef _H_CLUX_PARSER_AST_NIL_
#define _H_CLUX_PARSER_AST_NIL_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * nil 字面量节点（无额外字段）。
 * nil 是内置类型（与 type 类似）的唯一值：函数 0 初始化占位，
 * 未来表示空指针。仅字面量值，不可作变量类型（var a:nil 非法）。
 * 可显式转换为 u64(0) 与任意 function 类型。
 */
static inline ast_node_t *ast_nil_new(arena_t *arena,
                                      uint32_t tok_begin, uint32_t tok_end) {
    ast_node_t *n = (ast_node_t *)arena_calloc(
        arena, 1, sizeof(ast_node_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->kind      = AST_NIL;
    n->tok_begin = tok_begin;
    n->tok_end   = tok_end;
    return n;
}

/**
 * 解析 nil 字面量（nil 关键字）。
 * 不匹配返回 NULL。
 */
ast_node_t *parse_nil(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
