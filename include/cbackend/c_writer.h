#ifndef _H_CLUX_CBACKEND_C_WRITER_
#define _H_CLUX_CBACKEND_C_WRITER_

#include "cbackend/c_ir.h"
#include "core/allocator.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单文件输出（无头文件）：所有类型/声明都写进一个 .c（调试/兼容用）。 */
int cir_write_file(const cir_unit_t *unit, const char *path,
                   allocator_t *alloc);

/**
 * 编译单元输出：`h_path` 非 NULL 时生成 `.h` + `.c` 配对——
 *   - `.h`：include guard、C ABI typedef、头文件类型（拓扑序）、
 *           `export` 的函数/cfunc/全局变量声明（extern）；
 *   - `.c`：`#include "<h basename>"` + 其余类型、static 函数/全局、
 *           运行期辅助函数与入口。
 * 导出的函数/全局在 `.c` 里不加 `static`；其余一律 `static`。
 */
int cir_write_pair(const cir_unit_t *unit, const char *c_path,
                   const char *h_path, allocator_t *alloc);

/** 递归标记"必须进头文件"的类型（导出声明签名 → 依赖闭包）。 */
void cw_mark_header_type(cir_type_t *t);

#ifdef __cplusplus
}
#endif
#endif
