#ifndef _H_CLUX_PARSER_AST_REEXPORT_
#define _H_CLUX_PARSER_AST_REEXPORT_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* 代理导出节点：export * from "<path>";
 *
 * 语义（m5-design §1.3）：把依赖模块的**全部导出符号**原样并入本模块导出表；
 * 不支持选择性代理（`export { x } from "..."`）。若转发符号与本模块已有导出
 * 同名 → 编译错误（符号冲突）。
 *
 * 与 AST_IMPORT 一样由 sema pass0 处理：递归编译依赖模块（其符号随之注册进
 * vm->modules）；compile_module 构建本模块导出表时重解析本节点路径，把依赖
 * 模块的导出符号并入本模块（不注册命名空间别名）。
 *
 * path: 模块路径（字符串字面量内容，引号已去除；与 AST_IMPORT 同款处理）。
 */
typedef struct {
    ast_node_t  base;        /* kind = AST_REEXPORT */
    strslice_t  path;        /* 被代理模块路径（字符串字面量内容） */
} ast_reexport_t;

static inline ast_node_t *ast_reexport_new(arena_t *arena,
                                           uint32_t tok_begin, uint32_t tok_end) {
    ast_reexport_t *n = (ast_reexport_t *)arena_calloc(
        arena, 1, sizeof(ast_reexport_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_REEXPORT;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析代理导出 export * from "<path>";（调用点已确认 `export` 关键字） */
ast_node_t *parse_reexport(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_REEXPORT_ */
