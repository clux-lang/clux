#ifndef _H_CLUX_VM_TYPE_OWNERSHIP_
#define _H_CLUX_VM_TYPE_OWNERSHIP_

#include "vm/ownership.h"
#include "vm/type.h"

/* 编译期所有权已移除：保留最小空实现给旧调用点，类型层不再产生修饰类型。 */

static inline const type_t *type_ownership_apply(vm_t *vm, ownership_t own,
                                                 const type_t *t) {
    (void)vm;
    (void)own;
    return t;
}

static inline ownership_t type_ownership_of(const type_t *t) {
    (void)t;
    return OWN_NONE;
}

static inline const type_t *type_ownership_sub(const type_t *t) {
    (void)t;
    return NULL;
}

#endif /* _H_CLUX_VM_TYPE_OWNERSHIP_ */
