#ifndef _H_CLUX_VM_VALUE_OWNCOPY_
#define _H_CLUX_VM_VALUE_OWNCOPY_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/value.h"

/**
 * 把一个值写入**新分配的聚合块**（构造表达式的字段/元素、slice 字面量元素、
 * option 的 value 字段）。
 *
 * 规则：`ft` **持有资源**（含 own 字段/元素，或本身是 own/share/weak 指针）时
 * **深拷贝**，否则平凡 memcpy。
 *
 * 为什么不能一律 memcpy —— 那会让新块与**源临时值共享**内嵌堆块，"谁负责释放"
 * 就依赖上下文，两种情况都错：
 *   - 新块随后被 own 变量/形参**采用**（`var a = .T{...}`、实参传递）时，源临时值
 *     仍归作用域所有，作用域退出先释放、新块再释放同一个堆块 → **double-free**
 *     （实测 access violation）；
 *   - 反过来清空源，则在"新块不被采用"的路径上变成泄漏/悬垂。
 *
 * 深拷贝后新块是**独立所有者**、源临时值继续归作用域，各自释放各自的：不需要任何
 * "清空源"特例，也与该值后续被定义/传参/丢弃的方式无关。无 own 的字段仍走平凡
 * memcpy，整块构造保持 O(size)。
 *
 * `ft` 与 `src` 的类型可能不同（字面量 i32 → i64 字段等）——调用方须先
 * implicit_cast 到 `ft`；本函数一律以 `ft` 解释字节。返回值是 `dst` 内的地址。
 */
void *value_own_copy_into(vm_t *vm, void *dst, const value_t *src,
                          const type_t *ft);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VALUE_OWNCOPY_ */
