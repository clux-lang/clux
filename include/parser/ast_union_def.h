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

/* tag union 定义节点：union Name { field: type; ... }
 *
 * tag union（M2 §tag union）与 struct 同构语法：字段直接平铺，每个字段
 * 即一个 member（字段名 = tag 名，字段类型 = payload 类型）。字段链复用
 * AST_STRUCT_FIELD，以分号分隔（见 parse_union_def）。
 *
 * sema 登记为 union_type_t：tag 整数前缀 + 各 member payload 联合体布局。
 * 运行时：
 *   - 构造 .Value{.i = 42}：CONSTRUCT 收 tag 哨兵 + 单字段值（强制单字段）
 *   - 字段访问 v.i：FIELD_GET 运行时校验 tag，不符返回 error value（panic）
 *   - tag 判定 v is i：IS_TAG 指令比较 tag 整数
 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;            /* union 名 */
    ast_node_t *fields;          /* AST_STRUCT_FIELD 兄弟链（字段 = member） */
    ast_node_t *fields_last;
    uint32_t    type_id;         /* sema 登记的类型 id（TYPE_ID_PROGRAM_BASE+idx；
                                    compiler 顶层名字绑定 LOAD_TYPE 用） */
} ast_union_def_t;

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

/** 解析 tag union 定义 union Name { field: type; ... }（struct 同构平铺） */
ast_node_t *parse_union_def(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_UNION_DEF_ */
