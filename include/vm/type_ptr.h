#ifndef _H_CLUX_VM_TYPE_PTR_
#define _H_CLUX_VM_TYPE_PTR_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 指针类型 vtable（own/ref/fatal 三种所有权修饰共享同一结构，kind 区分） */
extern const vtable_t VTABLE_PTR_OWN;
extern const vtable_t VTABLE_PTR_REF;
extern const vtable_t VTABLE_PTR_FATAL;

/* ================================================================ */
/* 指针类型（ptr_type_t，继承 type_t，m3-design §3）                  */
/* ================================================================ */

/**
 * ptr_type_t 构造对应统一两遍协议 PUSH_PTR / DEFINE_TYPE / LOAD_TYPE /
 * SET_TYPE（设 base_type）/ SEAL（与 func 签名类型同族）：
 *   - type_ptr_push 分配空 ptr_type（base_type=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_ptr_set_base 设被指向类型 T（SET_TYPE 运行期用）；密封后静默忽略。
 *   - type_ptr_seal 按 base_type 指针去重 intern（同一 base 的三种所有权是
 *     三个独立实例），标记 sealed。
 *
 * 指针类型的 size/align 恒定（指针宽），密封不依赖 base_type 布局——hoist
 * 中属引用依赖（emit_ref_type，LOAD_TYPE 拉回 base，不递归），与 func
 * 签名类型同族。
 */
typedef struct ptr_type_t ptr_type_t;  /* 定义见 vm/type.h */

/** PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。
 *  kind = 所有权修饰（TYPE_KIND_PTR_OWN/REF/FATAL，PUSH_PTR 指令 u8 操作数）。 */
const type_t *type_ptr_push(vm_t *vm, type_kind_t kind);

/** SET_TYPE 运行期用：设被指向类型 T（密封后静默忽略） */
void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base);

/** SEAL：按 base_type 去重 intern + 置 sealed。返回该 const type */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（type_ptr_push + set_base + seal）：sema 解析 own/ref/fatal *T
 *  用。不向操作数栈压 type value（嵌套构造场景避免栈布局污染）。 */
const type_t *type_ptr_intern(vm_t *vm, type_kind_t kind, const type_t *base);

/** 取指针类型的被指向类型 T（非指针返回 NULL） */
static inline const type_t *ptr_type_base(const type_t *t) {
    return (t && (t->kind == TYPE_KIND_PTR_OWN ||
                  t->kind == TYPE_KIND_PTR_REF ||
                  t->kind == TYPE_KIND_PTR_FATAL))
               ? ((const ptr_type_t *)t)->base_type
               : NULL;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_PTR_ */
