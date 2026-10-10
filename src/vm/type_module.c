#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"

/* module value 的 data 布局: module_t* 指针（借用，不拥有）。
 * 模块值是命名空间单例引用——IMPORT 构造，GET_MEMBER 从中取成员。
 * 不可实例化/赋值/传递（仅由 IMPORT 指令内部构造）。 */

/* ---- clone ---- */

static value_t *module_clone(vm_t *vm, value_t *v) {
    /* 模块值是轻量指针包装——clone 复制指针（共享 module_t*，借用） */
    void *src = value_data(v);
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), &src);
    return value_make(vm, value_type(v), data);
}

const vtable_t VTABLE_MODULE = {
    .clone = module_clone,
};
