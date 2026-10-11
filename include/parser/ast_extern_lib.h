#ifndef _H_CLUX_PARSER_AST_EXTERN_LIB_
#define _H_CLUX_PARSER_AST_EXTERN_LIB_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* extern lib 声明节点（M6 FFI）：
 *
 *     extern lib "<路径>" as <名>;
 *     extern lib "libc.so.6" as libc;
 *
 * **判据是关键字 `extern lib`，不是文件后缀**：路径只是"交给运行期
 * dlopen/LoadLibrary 的加载参数"，同一路径也可能是普通数据或源码模块，
 * 因此不做任何后缀判类、也不因此产生"看起来像动态库"的诊断。
 * `import`（AST_IMPORT）语义保持不变——只表示源码模块。
 *
 * 该库名随后被 `extern cfunc <库>::<函数>(...)` 引用（sema/M6 Phase 3+）。
 * 与 import 一样，compiler 不为本声明生成运行期字节码：加载在 sema 阶段
 * 完成（dlopen 句柄存 module_t->dl_handle）。
 */
typedef struct {
    ast_node_t  base;   /* kind = AST_EXTERN_LIB */
    strslice_t  alias;  /* 库别名（命名空间，extern cfunc 以 <alias>::<fn> 引用） */
    strslice_t  path;   /* 动态库路径（字符串字面量内容，去引号） */
} ast_extern_lib_t;

static inline ast_node_t *ast_extern_lib_new(arena_t *arena, uint32_t tok_begin,
                                             uint32_t tok_end) {
    ast_extern_lib_t *n = (ast_extern_lib_t *)arena_calloc(
        arena, 1, sizeof(ast_extern_lib_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_EXTERN_LIB;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 `extern lib "<path>" as <alias>;`（调用方已消费 `extern` 关键字，tb 为其下标） */
ast_node_t *parse_extern_lib(parser_t *p, uint32_t tb);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_EXTERN_LIB_ */