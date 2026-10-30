#ifndef _H_CLUX_VM_TYPE_PTR_
#define _H_CLUX_VM_TYPE_PTR_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 指针类型 vtable */
extern const vtable_t VTABLE_PTR;

typedef struct ptr_type_t ptr_type_t;  /* 定义见 vm/type.h */

/** PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。 */
const type_t *type_ptr_push(vm_t *vm);

/** SET_TYPE 运行期用：设被指向类型 T（密封后静默忽略） */
void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base);

/** SEAL：按 base_type 去重 intern + 置 sealed。返回该 const type */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（push + set_base + seal）：sema 解析 *T 用。 */
const type_t *type_ptr_intern(vm_t *vm, const type_t *base);

/** 取指针类型的被指向类型 T（非指针返回 NULL） */
static inline const type_t *ptr_type_base(const type_t *t) {
    return type_is_ptr(t) ? ((const ptr_type_t *)t)->base_type : NULL;
}

/** 指针值。 */
typedef struct ptr_value_t {
    void *ptr;
} ptr_value_t;

/** 读指针值目标地址（非指针 value 返回 NULL）。 */
static inline void *ptr_value_target(const value_t *v) {
    if (!v) return NULL;
    const type_t *t = value_type(v);
    if (!type_is_ptr(t))
        return NULL;
    return ((const ptr_value_t *)value_data(v))->ptr;
}

/** 构造指针值。 */
value_t *ptr_make_value(vm_t *vm, const type_t *pt, void *target);

/* ---- 递归销毁/复制（闭包 env、复合值）---- */

/** 目标类型是否含需递归扫描的 own 字段。 */
bool ptr_type_needs_scan(const type_t *t);

void ptr_free_recursive(vm_t *vm, const type_t *t, void *data);

/** 递归清空 data 块内 own/fatal 指针的 ptr（不释放）。 */
void ptr_clear_recursive(vm_t *vm, const type_t *t, void *data);

/** 深拷贝 type 类型的数据块。 */
void *ptr_clone_block(vm_t *vm, const type_t *type, const void *src);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_PTR_ */
