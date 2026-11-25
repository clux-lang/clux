#ifndef _H_ENGINE_DIAG_
#define _H_ENGINE_DIAG_
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/string.h"
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 诊断项（sema 错误收集通道的元素）
 *
 * sema 阶段不一个错误立即返回：错误逐层抛出到 stmt 级节点，由 context
 * 托管的 diags 向量收集，然后从下一个 stmt 继续解析——一次编译产出全部
 * 诊断。diag 项 = 级别 + clux_error_t（位置 + 消息 + Rust 风格渲染复用
 * core/error 通道）。与 exec 的 error 值通道互不相交：error 值在作用域
 * 链上流动（借用 err 指针），diag 本体归 context 的 diags 向量持有。
 * =========================================================================== */

typedef enum {
  CLUX_DIAG_ERROR,   /* 硬错误：编译终止 */
  CLUX_DIAG_WARNING, /* 警告：不终止编译 */
  CLUX_DIAG_NOTE,    /* 提示：附加信息 */
} clux_diag_level_t;

typedef struct _clux_diag_t clux_diag_t;

/* 级别的小写名（"error"/"warning"/"note"），渲染前缀用。 */
const char *clux_diag_level_name(clux_diag_level_t level);

/* 构造诊断项：消息按 printf 语义格式化，loc 允许 NULL（零位置）。
   error 本体归 diag 持有，delete_clux_diag 一并释放。 */
clux_diag_t *create_clux_diag(allocator_t *allocator,
                              clux_diag_level_t level,
                              const location_t *loc,
                              const char *fmt,
                              ...);

/* va_list 版本（context 的 printf 风格入口经它转发）。 */
clux_diag_t *create_clux_diagv(allocator_t *allocator,
                               clux_diag_level_t level,
                               const location_t *loc,
                               const char *fmt,
                               va_list args);

/* 销毁诊断项并置 NULL（含 error 本体）。NULL/空指针安全。 */
void delete_clux_diag(clux_diag_t **diag);

clux_diag_level_t clux_diag_get_level(const clux_diag_t *self);

/* 借用 error 本体（位置/消息/源码渲染走 core/error 访问器）。 */
const clux_error_t *clux_diag_get_error(const clux_diag_t *self);

/* 渲染为 Rust 风格诊断块（首行级别前缀 + error_to_string_level 主体）。
   返回的 string_t 由调用方释放。 */
string_t *clux_diag_to_string(clux_diag_t *self, allocator_t *allocator);

#ifdef __cplusplus
}
#endif
#endif
