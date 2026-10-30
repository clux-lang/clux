#ifndef _H_CLUX_VM_VALUE_DEEPCOPY_
#define _H_CLUX_VM_VALUE_DEEPCOPY_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/value.h"

/**
 * 为 `t` 分配一块新 data 并写入 `src_data` 的内容：
 * `t` **持有资源**时走**对象图克隆**（`ptr_clone_block`），否则平凡 memcpy。
 *
 * 用于各复合类型的隐式转换/身份拷贝分支。**为什么不能一律浅拷贝**：源与目标
 * 常常是"同布局但不同 type_t 实例"（构造产出的 base 实例 vs hoist 物化 / sema
 * 剥离出的 base 实例），浅拷贝会让新值与源**共享内嵌 own 堆块**，两侧析构各
 * 释放一次 → double-free（实测 access violation）。
 *
 * 不含 own 的类型仍走 memcpy，保持原有的零开销身份拷贝。
 */
void *value_alloc_data_own(vm_t *vm, const type_t *t, const void *src_data);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VALUE_DEEPCOPY_ */
