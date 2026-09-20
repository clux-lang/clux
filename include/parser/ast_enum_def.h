#ifndef _H_CLUX_PARSER_AST_ENUM_DEF_
#define _H_CLUX_PARSER_AST_ENUM_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* 枚举定义节点：enum Name:Underlying { Var = val, ... }
 *
 * underlying_type 为底层类型表达式（AST_IDENT，如 i32；sema 折叠为
 * AST_TYPE_REF）。variants 为 AST_ENUM_VARIANT 兄弟链（name = value）。
 * 每个 variant 值必须显式写出（缺省报错，见 parse_enum_def）。
 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;            /* 枚举名 */
    ast_node_t *underlying_type; /* 底层类型表达式（AST_IDENT） */
    ast_node_t *variants;        /* AST_ENUM_VARIANT 兄弟链 */
    ast_node_t *variants_last;
    uint32_t    type_id;         /* sema 登记的类型 id（TYPE_ID_PROGRAM_BASE+idx；
                                    compiler 顶层名字绑定 LOAD_TYPE 用） */
} ast_enum_def_t;

/* 单个 variant 声明（独立节点，兄弟链挂接在 ast_enum_def_t::variants） */
typedef struct {
    ast_node_t  base;    /* kind = AST_ENUM_VARIANT */
    strslice_t  name;    /* variant 名 */
    ast_node_t *value;   /* = 后的值表达式 */
} ast_enum_variant_t;

static inline ast_node_t *ast_enum_def_new(arena_t *arena,
                                           uint32_t tok_begin, uint32_t tok_end) {
    ast_enum_def_t *n = (ast_enum_def_t *)arena_calloc(
        arena, 1, sizeof(ast_enum_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_ENUM_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

static inline ast_node_t *ast_enum_variant_new(arena_t *arena,
                                               uint32_t tok_begin,
                                               uint32_t tok_end) {
    ast_enum_variant_t *n = (ast_enum_variant_t *)arena_calloc(
        arena, 1, sizeof(ast_enum_variant_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_ENUM_VARIANT;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析枚举定义 enum Name:Underlying { Var = val, ... } */
ast_node_t *parse_enum_def(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_ENUM_DEF_ */
