#ifndef _H_CLUX_PARSER_AST_CONSTRUCT_
#define _H_CLUX_PARSER_AST_CONSTRUCT_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * 类型字面量构造节点：.<type> { f1, f2, ... }
 *   - type：类型表达式（AST_IDENT / AST_ARRAY / AST_CONST ...）
 *   - fields：逗号分隔的字段值表达式兄弟链（head/tail）
 * 构造语法必须 '.' 前导（与 %v 调试输出 '.type{value}' 一致）。
 * 由 parse_unary 的 '.' 前缀分支构造；下游 sema/compiler 暂以 default 分支容错。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *type;          /* 类型表达式（如 AST_IDENT / AST_ARRAY） */
    ast_node_t *fields;        /* 字段值表达式兄弟链（head） */
    ast_node_t *fields_last;   /* 字段值表达式兄弟链（tail） */
} ast_construct_t;

static inline ast_node_t *ast_construct_new(arena_t *arena,
                                            uint32_t tok_begin, uint32_t tok_end) {
    ast_construct_t *n = (ast_construct_t *)arena_calloc(
        arena, 1, sizeof(ast_construct_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_CONSTRUCT;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_CONSTRUCT_ */
