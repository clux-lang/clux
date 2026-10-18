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
 *     extern lib <别名> from "<库名>";
 *     extern lib libc from "c";        // → c.dll / libc.so / c.so（按平台）
 *     extern lib libc from "*";        // 当前进程自身（RTLD_DEFAULT 语义）
 *     extern lib td   from "clux_ffi_testdata";   // 通用名，找平台文件
 *     extern lib x    from "/abs/libfoo.so";      // 显式路径也接受
 *
 * **`<库名>` 是通用名（generic name），不是文件名**：加载器按平台展开候选
 * （Windows `name.dll`；Linux `libname.so` / `name.so`；macOS 再加 `.dylib`）
 * 逐个尝试——与 CMake 的同名风格一致，源码因此跨平台。显式路径/文件名同样
 * 接受（加载器先按 as-is 尝试）。判据仍是关键字 `extern lib`，加载器不按
 * 后缀判类，无法加载时由操作系统给原因。
 *
 * 该库名随后被 `extern cfunc <库>::<函数>(...)` 引用。与 import 一样，
 * compiler 不为本声明生成运行期字节码：加载在编译/加载期完成，句柄存进
 * module_t->ffi_libs；`.cxb`/`.cxs` 容器里保存的是**通用名**（不是解析后的
 * 绝对路径），产物因此可跨平台搬运。
 */
typedef struct {
    ast_node_t  base;   /* kind = AST_EXTERN_LIB */
    strslice_t  alias;  /* 库别名（命名空间，extern cfunc 以 <alias>::<fn> 引用） */
    strslice_t  name;   /* 通用库名 / 显式路径（字符串字面量内容，去引号） */
    /* 可选的 `assert(<条件>)` 条件后缀（规范化文本；空 = 无条件）。
       条件为假时**整条声明在前脱糖阶段被裁剪**（见 core/ffi_cond.h）。 */
    strslice_t  cond;
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

/** 解析 `extern lib <alias> from "<name>" [assert(<cond>)];`（调用方已消费
 *  `extern`，tb 为其下标）。`assert` 是**软关键字**（该位置按文本匹配），
 *  条件文本经 ffi_cond_validate 立即校验，错误定位在声明上。 */
ast_node_t *parse_extern_lib(parser_t *p, uint32_t tb);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_EXTERN_LIB_ */