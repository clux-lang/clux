#ifndef _H_CLUX_VM_VALUE_DUMP_
#define _H_CLUX_VM_VALUE_DUMP_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/vm.h"
#include "vm/value.h"
#include "core/string.h"

/**
 * value_dump_string: 将 value 以 `.type { value }` 形式（复杂类型递归包裹）
 * 追加到 out 字符串，用于调试输出（如 printf %v）。
 *
 * - 基础类型输出值；bool → true/false；str → 带双引号字符串（`"hello"`）。
 * - 数组递归每个元素（`. [3]i32 { .i32{1}, .i32{2}, .i32{3} }`）。
 * - const / volatile 类型名保留完整限定名，值体按底层类型读取。
 * - 结构体当前 VM 未实现，best-effort 仅输出类型名 + 空 `{}`（前向兼容）。
 * - shadow value 与 NULL 安全（打印 `<shadow>` / `<null>`，不读 data）。
 */
void value_dump_string(const vm_t *vm, const value_t *v, string_t *out);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VALUE_DUMP_ */
