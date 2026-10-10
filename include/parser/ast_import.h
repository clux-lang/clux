#ifndef _H_CLUX_PARSER_AST_IMPORT_
#define _H_CLUX_PARSER_AST_IMPORT_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* import 语句节点：import <alias> from "<path>";
 *
 * alias: 用户指定的命名空间别名（合法标识符）
 * path: 模块路径（字符串字面量内容，仅支持相对路径 ./ 或 ../ 开头）
 *
 * sema 遇到此节点时递归编译被导入模块，从 module->exports 获取成员类型信息，
 * 在当前模块全局作用域注册 <alias> 为 MODULE 符号。
 * compiler 不对 import 语句生成字节码（模块加载在 sema 阶段完成，
 * 运行期 IMPORT 指令在 :: 成员访问时按需发出）。
 */
typedef struct {
    ast_node_t  base;        /* kind = AST_IMPORT */
    strslice_t  alias;       /* 命名空间别名 */
    strslice_t  path;        /* 模块路径（字符串字面量内容） */
} ast_import_t;

static inline ast_node_t *ast_import_new(arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end) {
    ast_import_t *n = (ast_import_t *)arena_calloc(
        arena, 1, sizeof(ast_import_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_IMPORT;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 import 语句：import <alias> from "<path>"; */
ast_node_t *parse_import(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_IMPORT_ */
