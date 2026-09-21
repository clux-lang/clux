#ifndef _H_CLUX_PARSER_AST_STRUCT_DEF_
#define _H_CLUX_PARSER_AST_STRUCT_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* 结构体定义节点：struct Name { field: type; ... }
 *
 * fields 为 AST_STRUCT_FIELD 兄弟链（name: type，分号分隔，与 enum variant
 * 的逗号分隔不同——见 M2 设计 §2）。字段类型表达式（AST_IDENT/类型修饰等）
 * sema 阶段折叠为类型引用。
 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;            /* 结构体名 */
    ast_node_t *fields;          /* AST_STRUCT_FIELD 兄弟链 */
    ast_node_t *fields_last;
    uint32_t    type_id;         /* sema 登记的类型 id（TYPE_ID_PROGRAM_BASE+idx；
                                    compiler 顶层名字绑定 LOAD_TYPE 用） */
} ast_struct_def_t;

/* 单个字段声明（独立节点，兄弟链挂接在 ast_struct_def_t::fields） */
typedef struct {
    ast_node_t  base;    /* kind = AST_STRUCT_FIELD */
    strslice_t  name;    /* 字段名 */
    ast_node_t *type;    /* 字段类型表达式 */
} ast_struct_field_t;

static inline ast_node_t *ast_struct_def_new(arena_t *arena,
                                             uint32_t tok_begin,
                                             uint32_t tok_end) {
    ast_struct_def_t *n = (ast_struct_def_t *)arena_calloc(
        arena, 1, sizeof(ast_struct_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_STRUCT_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

static inline ast_node_t *ast_struct_field_new(arena_t *arena,
                                               uint32_t tok_begin,
                                               uint32_t tok_end) {
    ast_struct_field_t *n = (ast_struct_field_t *)arena_calloc(
        arena, 1, sizeof(ast_struct_field_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_STRUCT_FIELD;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析结构体定义 struct Name { field: type; ... } */
ast_node_t *parse_struct_def(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_STRUCT_DEF_ */
