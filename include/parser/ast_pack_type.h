#ifndef _H_CLUX_PARSER_AST_PACK_TYPE_
#define _H_CLUX_PARSER_AST_PACK_TYPE_

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 泛型参数包的**函数参数类型**（M7 §30.2）：
 *
 *     func print[...Args](fmt: str, args: ...Args): void;
 *
 * `name` 指向泛型参数表中的参数包名。实例化时该参数被展开为 N 个独立参数，
 * 函数体入口生成 `var args: <T1,...,TN> = .{...};` 聚合（设计 §30.3）。
 * 仅允许出现在函数参数表的**最后一个参数**上。 */
typedef struct {
    ast_node_t base;    /* kind = AST_PACK_TYPE */
    strslice_t name;    /* 参数包名（Args） */
} ast_pack_type_t;

static inline ast_node_t *ast_pack_type_new(arena_t *arena,
                                            uint32_t tok_begin,
                                            uint32_t tok_end) {
    ast_pack_type_t *n = (ast_pack_type_t *)arena_calloc(
        arena, 1, sizeof(ast_pack_type_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_PACK_TYPE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif
