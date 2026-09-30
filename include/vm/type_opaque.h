#ifndef _H_CLUX_VM_TYPE_OPAQUE_
#define _H_CLUX_VM_TYPE_OPAQUE_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** opaque 类型 vtable（≈ C 的 void*，m3-design §8.4） */
extern const vtable_t VTABLE_OPAQUE;

/**
 * opaque 是内建单例类型（vm->type_opaque，id 17，无开放构造阶段——与
 * error/interrupt 同族：vm_init_builtins 创建即 sealed，无 type_seal 槽位）。
 *
 * 运算行为（m3-design §8.4）：
 *   - clone/assign：指针值平凡拷贝（裸指针数据）。
 *   - dispose：no-op（Step A 静默）。
 *   - implicit_cast：任意指针 → opaque（身份拷贝，由指针 vtable 提供）。
 *   - explicit_cast：opaque → 任意指针（as），由指针 vtable 提供。
 *   - type_equal/type_extends：单例身份比较（opaque 是独立类型）。
 */
static inline bool type_is_opaque(const type_t *t) {
    return t && t->kind == TYPE_KIND_OPAQUE;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_OPAQUE_ */
