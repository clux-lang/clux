#ifndef _H_CLUX_PARSER_AST_UNION_DEF_
#define _H_CLUX_PARSER_AST_UNION_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* tag union 定义节点：union Name { Tag: {field: type; ...}; Empty; ... }
 *
 * tag union（M2 §tag union）：member 名 = tag 名。每个 member 可携带
 * 匿名 struct payload（{field: type; ...}，字段链复用 AST_STRUCT_FIELD）或
 * 无 payload（纯 tag，Empty;）。成员以分号分隔，payload 字段以逗号/分号
 * 分隔（见 parse_union_def）。
 *
 * sema 登记为 union_type_t：tag 整数前缀 + 各 member 的联合体布局。
 * 运行时：
 *   - 构造 .Shape{.Circle{...}}：外层 union 构造 + 内层 member struct 构造
 *   - 字段访问 s.radius：FIELD_GET 运行时查 tag，不符返回 error value
 *   - tag 判定 s is Circle：IS_TAG 指令比较 tag 整数
 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;            /* union 名 */
    ast_node_t *members;         /* AST_UNION_MEMBER 兄弟链 */
    ast_node_t *members_last;
    uint32_t    type_id;         /* sema 登记的类型 id（TYPE_ID_PROGRAM_BASE+idx；
                                    compiler 顶层名字绑定 LOAD_TYPE 用） */
} ast_union_def_t;

/* 单个 member 声明（独立节点，兄弟链挂接在 ast_union_def_t::members）。
 * fields 为 AST_STRUCT_FIELD 兄弟链（payload 字段，可为 NULL = 纯 tag）。 */
typedef struct {
    ast_node_t  base;    /* kind = AST_UNION_MEMBER */
    strslice_t  name;    /* tag/member 名 */
    ast_node_t *fields;  /* payload 字段链（AST_STRUCT_FIELD；NULL = 纯 tag） */
    ast_node_t *fields_last;
    uint32_t    tag;     /* sema 分配的 tag 整数值（0..member_count-1） */
} ast_union_member_t;

static inline ast_node_t *ast_union_def_new(arena_t *arena,
                                            uint32_t tok_begin,
                                            uint32_t tok_end) {
    ast_union_def_t *n = (ast_union_def_t *)arena_calloc(
        arena, 1, sizeof(ast_union_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_UNION_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

static inline ast_node_t *ast_union_member_new(arena_t *arena,
                                               uint32_t tok_begin,
                                               uint32_t tok_end) {
    ast_union_member_t *n = (ast_union_member_t *)arena_calloc(
        arena, 1, sizeof(ast_union_member_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_UNION_MEMBER;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 tag union 定义 union Name { Tag: {fields}; Empty; ... } */
ast_node_t *parse_union_def(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_UNION_DEF_ */
