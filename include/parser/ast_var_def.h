#ifndef _H_CLUX_PARSER_AST_VAR_DEF_
#define _H_CLUX_PARSER_AST_VAR_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>

typedef struct {
    ast_node_t  base;
    strslice_t  name;        /* 变量名 */
    ast_node_t *type_expr;   /* 类型表达式（NULL = 推断；AST_IDENT/AST_CONST/AST_VOLATILE） */
    ast_node_t *init;        /* 初始化表达式（必须存在；AST_UNDEF = 未初始化声明） */
    bool        is_comptime; /* comptime var：右值必须编译期可计算，引用点折叠为常量 */

    /* ---- M6 FFI：`extern var <库>::<名>: T;`（动态库导出的**全局数据符号**）----
     * 与 `extern cfunc` 对称、与 `extern lib` 配套：名字是库里的数据符号，
     * 运行期由 dlsym/GetProcAddress 得到**变量地址**，读写都作用于库的那块内存
     * （没有本地副本）。因此：
     *   - `init` 恒为 NULL（**没有初始化器**，语义是声明而非定义）；
     *   - 是**左值**：`crt::_fmode = 0;` 走 EXTERN_STORE（区别于只读的常量）。
     * `lib_name` 是 `extern lib` 的别名（与 ast_func_def_t 的同名字段一致）。 */
    bool        is_extern_var;
    strslice_t  lib_name;
} ast_var_def_t;

static inline ast_node_t *ast_var_def_new(arena_t *arena,
                                          uint32_t tok_begin, uint32_t tok_end) {
    ast_var_def_t *n = (ast_var_def_t *)arena_calloc(
        arena, 1, sizeof(ast_var_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_VAR_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析变量定义 var name[:type] = init; 初始化必须存在。 */
ast_node_t *parse_var_def(parser_t *p);

/** 解析 `extern var <lib>::<name>: <T>;`（调用方已消费 `extern`）。
 * M6 FFI：声明动态库导出的全局数据符号；无初始化器，符号由 dlsym 在运行期解析。
 * 别名来自 `extern lib "..." as <lib>;`（路径 `"*"` = 当前进程自身）。 */
ast_node_t *parse_extern_var(parser_t *p, uint32_t tb);

#ifdef __cplusplus
}
#endif
#endif
