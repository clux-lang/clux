#ifndef _H_CLUX_PARSER_AST_CUNION_DEF_
#define _H_CLUX_PARSER_AST_CUNION_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* C 语义 union 定义节点：cunion Name { field: type; ... }
 *
 * cunion（M2 §cunion）与 struct 同构语法：字段直接平铺，每个字段即一个
 * member（字段名 = member 名，字段类型 = member 类型）。字段链复用
 * AST_STRUCT_FIELD，以分号分隔（见 parse_cunion_def）。
 *
 * 与 tag union 的本质区别：cunion 无 tag——所有 member 共享内存 offset 0，
 * 不做安全检查（开发者自负安全，对齐 C union FFI 布局）。sema 登记为
 * cunion_type_t：size=max(member size)，align=max(member align)。
 * 运行时：
 *   - 构造 .C{.f = v}：CONSTRUCT 收 member 下标哨兵 + 单字段值（强制单字段）
 *   - 字段访问 v.f：FIELD_GET/FIELD_SET 按名反查，无 tag 校验，直接读写
 *     offset 0（共享内存直接覆盖）
 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;            /* cunion 名 */
    ast_node_t *fields;          /* AST_STRUCT_FIELD 兄弟链（字段 = member） */
    ast_node_t *fields_last;
    uint32_t    type_id;         /* sema 登记的类型 id（TYPE_ID_PROGRAM_BASE+idx；
                                    compiler 顶层名字绑定 LOAD_TYPE 用） */
} ast_cunion_def_t;

static inline ast_node_t *ast_cunion_def_new(arena_t *arena,
                                             uint32_t tok_begin,
                                             uint32_t tok_end) {
    ast_cunion_def_t *n = (ast_cunion_def_t *)arena_calloc(
        arena, 1, sizeof(ast_cunion_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_CUNION_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 C 语义 union 定义 cunion Name { field: type; ... }（struct 同构平铺） */
ast_node_t *parse_cunion_def(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_CUNION_DEF_ */
