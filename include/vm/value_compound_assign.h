#ifndef _H_CLUX_VM_VALUE_COMPOUND_ASSIGN_
#define _H_CLUX_VM_VALUE_COMPOUND_ASSIGN_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/value.h"

/**
 * 复合类型**就地赋值**的共享实现（struct/tuple/array/option/union 的 vtable
 * `assign` 槽位）。
 *
 * 语义（与 `own *T` 的 ptr_assign 对齐，且是**唯一**正确形态）：
 *   1. **释放左值旧值**拥有的堆块（`ptr_free_recursive`）——不释放就是
 *      泄漏（整块 memcpy 覆盖会丢掉旧字段的裸指针，再也找不到那些块）；
 *   2. 整块 memcpy（`dst->data` 是全平凡字节块，字段即裸字节）；
 *   3. **清空源**的 own/fatal 指针（`ptr_clear_recursive`）——所有权随
 *      memcpy 转移进 dst，源不再持有。不清空则源（临时值或具名变量）与 dst
 *      共享同一堆块 → 两侧析构各释放一次 = **double-free**（实测 access
 *      violation）。
 *
 * 为什么第 3 步对"具名变量源"也是对的：源变量的 data **块**仍归它的作用域
 * （清空只把块内的裸指针置 NULL，块本身与作用域的书本账不变），而它的资源已经
 * 交给 dst。sema 侧保证只允许**将亡值**（fatal：move/clone/构造产物/返回值）或
 * 普通可拷贝值作为右值；前者本来就该被消费，后者不含 own 资源（清空是空转）。
 *
 * 调用方须先完成类型对齐（同类型，或已 `implicit_cast` 到左值类型）。
 * shadow 输入直接返回 dst（sema 纯类型检查路径）。
 */
value_t *compound_assign_transfer(vm_t *vm, value_t *dst, value_t *src);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VALUE_COMPOUND_ASSIGN_ */
