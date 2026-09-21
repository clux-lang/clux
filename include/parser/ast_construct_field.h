#ifndef _H_CLUX_PARSER_AST_CONSTRUCT_FIELD_
#define _H_CLUX_PARSER_AST_CONSTRUCT_FIELD_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * struct 构造具名字段节点：.{ type }{ .field = value, ... } 中的单个字段。
 * 具名字段 = struct 构造的字段形态（字段名 + 值表达式）；匿名构造 .{...}
 * 的字段同样以此节点表达（字段名匹配目标 struct 字段）。array/option 构造
 * 的字段是普通值表达式（无 AST_CONSTRUCT_FIELD 包装）。
 *
 * 由 parse_construct_field 在识别 `.IDENT =` 模式时构造；sema 校验字段名
 * 存在且字段数完全显式；compiler 只取 value 编译。
 */
typedef struct {
    ast_node_t  base;   /* kind = AST_CONSTRUCT_FIELD */
    strslice_t  name;   /* 字段名 */
    ast_node_t *value;  /* = 后的值表达式 */
} ast_construct_field_t;

static inline ast_node_t *ast_construct_field_new(arena_t *arena,
                                                  uint32_t tok_begin,
                                                  uint32_t tok_end) {
    ast_construct_field_t *n = (ast_construct_field_t *)arena_calloc(
        arena, 1, sizeof(ast_construct_field_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_CONSTRUCT_FIELD;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_CONSTRUCT_FIELD_ */
