#include "vm/type_error.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "vm/str_pool.h"
#include "core/panic.h"

#include <string.h>

/* ===========================================================================
 * error value 的 data 布局：error_data_t 内联（直接存结构体，非指针）
 *
 * 平凡化后 error_data_t 的 message/location 为 vm 字符串池内指针
 * （const char *，本体归 vm 字符串池），data 块 memcpy 可拷贝：
 *   - dispose：no-op（池内存归池，value_dispose 释放 data 块）
 *   - clone：alloc + memcpy
 * =========================================================================== */

static void error_dispose(vm_t *vm, value_t *v) {
    /* data 全平凡：message/location 为池引用，无需释放 */
    (void)vm; (void)v;
}

static value_t *error_clone(vm_t *vm, value_t *v) {
    /* shadow：返回新 shadow（error 不参与运算，防御性支持） */
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
    return value_make(vm, value_type(v), data);
}

const vtable_t VTABLE_ERROR = {
    .dispose = error_dispose,
    .clone   = error_clone,
};
