#ifndef _H_CLUX_PARSER_AST_ENUM_REF_
#define _H_CLUX_PARSER_AST_ENUM_REF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* 枚举 variant 引用节点：Color::Red
 *
 * type_expr 为枚举类型名（AST_IDENT；sema 折叠为 AST_TYPE_REF，compiler 发
 * LOAD_TYPE <id>）。variant 为 variant 名。sema 折叠时把 enum_type_find_variant
 * 查到的底层整数值写入 value 字段（compiler 发 PUSH_I* <value> + MAKE_ENUM）。
 * 未找到 variant 时 sema 报错并置 value=0（错误恢复产物）。
 */
typedef struct {
    ast_node_t  base;        /* kind = AST_ENUM_REF */
    ast_node_t *type_expr;   /* 枚举类型名（AST_IDENT → sema 折叠为 AST_TYPE_REF） */
    strslice_t  variant;     /* variant 名 */
    int64_t     value;       /* sema 折叠后的底层整数值 */
} ast_enum_ref_t;

static inline ast_node_t *ast_enum_ref_new(arena_t *arena,
                                           uint32_t tok_begin, uint32_t tok_end) {
    ast_enum_ref_t *n = (ast_enum_ref_t *)arena_calloc(
        arena, 1, sizeof(ast_enum_ref_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_ENUM_REF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_ENUM_REF_ */
