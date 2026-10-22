#ifndef _H_CLUX_PARSER_AST_INTERFACE_DEF_
#define _H_CLUX_PARSER_AST_INTERFACE_DEF_

#include "core/strslice.h"
#include "parser/ast_generic_param.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M7 §31：`interface` 编译期实体（不产生 type_t / vtable / 运行时实体）。
 *
 *     interface IWriter[T] {
 *         var fd: i32;
 *         func write(self: *T, data: ref []u8);
 *         ...Base;                 // 成员复制（P6c）
 *     }
 *
 * members：AST_VAR_DEF（无 init，必须显式类型）与 AST_FUNC_DEF（无 body，
 * 签名只有 `params` / `return_expr`）的兄弟链。embeds：AST_STRUCT_EMBED 链
 * （`...Base;` 成员复制）。 */
typedef struct {
    ast_node_t          base;
    strslice_t          name;
    ast_generic_params_t generic;
    ast_node_t         *members;
    ast_node_t         *members_last;
    ast_node_t         *embeds;
    ast_node_t         *embeds_last;
} ast_interface_def_t;

static inline ast_node_t *ast_interface_def_new(arena_t *arena, uint32_t tb,
                                                uint32_t te) {
    ast_interface_def_t *n = (ast_interface_def_t *)arena_calloc(
        arena, 1, sizeof(ast_interface_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind = AST_INTERFACE_DEF;
    n->base.tok_begin = tb;
    n->base.tok_end = te;
    return &n->base;
}

/** 解析接口声明 `interface Name[T] { ... }`。 */
ast_node_t *parse_interface_def(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
