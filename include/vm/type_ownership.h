#ifndef _H_CLUX_VM_TYPE_OWNERSHIP_
#define _H_CLUX_VM_TYPE_OWNERSHIP_

#include "vm/ownership.h"
#include "vm/type.h"

#include <stddef.h>

/**
 * 所有权修饰类型（own / ref / fatal）的构造与查询。
 *
 * **纯编译期概念**：运行期一律剥离（type_strip_ownership），运行期指令只引用
 * 裸类型；三者与 const/volatile 同构（持 sub、布局同 sub、运算代理到 sub）。
 * 详见 docs/ownership-as-type.md。
 */

/** 三种 kind 共用一张 vtable（全部槽位代理到 sub）。 */
extern const vtable_t VTABLE_OWNERSHIP;

/** 开放构造（PUSH_OWN/REF/FATAL 用）：sub=NULL、不入池、不压栈。 */
ownership_type_t *type_ownership_push_open(vm_t *vm, type_kind_t kind);

/** 构造/去重 intern：kind 必须是 TYPE_KIND_OWN / REF / FATAL。 */
const type_t *type_ownership_intern(vm_t *vm, type_kind_t kind,
                                    const type_t *sub);

/** 若 own != OWN_NONE，返回 t 的所有权修饰类型（intern）；否则原样返回 t。 */
const type_t *type_ownership_apply(vm_t *vm, ownership_t own, const type_t *t);

/** 取修饰类型的 sub（own/ref/fatal）；无修饰返回 NULL。 */
const type_t *type_ownership_sub(const type_t *t);

/** 类型上携带的所有权（穿透 const/volatile）；无则 OWN_NONE。 */
ownership_t type_ownership_of(const type_t *t);

/* `type_strip_ownership` 定义在 type.h（static inline）——结构化访问器与
 * 运行期边界都要用，避免每个编译单元为一行循环多一次外部调用。 */

#endif /* _H_CLUX_VM_TYPE_OWNERSHIP_ */
