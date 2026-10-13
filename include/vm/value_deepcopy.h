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

/**
 * 同布局**重贴标签**（转移语义）：把 `v` 的字节按 `target` 解释，并清空源对象图
 * 里的 own/fatal 指针（不释放——所有权已随字节转移）。
 *
 * 用途：转移点（函数调用的 fatal 形参接收、fatal 返回值交回）产出的值带 **base**
 * 类型，而声明侧要的是 **fatal 变体**。两者逐字节同布局，只需换 `type_t`。
 *
 * 只对同 kind 且同 size 成立时执行；否则返回 NULL，调用方须自行给出诊断。
 */
value_t *value_relabel_into(vm_t *vm, value_t *v, const type_t *target);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VALUE_DEEPCOPY_ */
