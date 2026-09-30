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

/* ================================================================ */
/* 指针值布局（m3-design §7/§8）                                     */
/* ================================================================ */

/**
 * 指针值（data 块布局）。own/ref/fatal 三种指针的 value data 统一为
 * { 裸指针, owns 标志 }：
 *   - ptr：指向目标内存（new 堆块 / 栈上值 / 借用目标）
 *   - owns：true = 拥有目标内存，dispose 时释放（new 产物）；
 *           false = 借用（x.& 指向栈值、ref 派生、fatal 流转中），
 *           dispose 跳过（防止释放栈值 / 双释放）
 *
 * size = sizeof(ptr_value_t)（对齐到指针宽），type_ptr_seal 设定。
 */
typedef struct ptr_value_t {
    void *ptr;
    bool  owns;
} ptr_value_t;

/** 读指针值目标地址（非指针 value 返回 NULL） */
static inline void *ptr_value_target(const value_t *v) {
    if (!v) return NULL;
    const type_t *t = value_type(v);
    if (!t || (t->kind != TYPE_KIND_PTR_OWN &&
               t->kind != TYPE_KIND_PTR_REF &&
               t->kind != TYPE_KIND_PTR_FATAL))
        return NULL;
    return ((const ptr_value_t *)value_data(v))->ptr;
}

/** 指针值是否拥有目标内存（new 产物 owns=true；借用/流转 owns=false） */
static inline bool ptr_value_owns(const value_t *v) {
    if (!v) return false;
    const type_t *t = value_type(v);
    if (!t || (t->kind != TYPE_KIND_PTR_OWN &&
               t->kind != TYPE_KIND_PTR_REF &&
               t->kind != TYPE_KIND_PTR_FATAL))
        return false;
    return ((const ptr_value_t *)value_data(v))->owns;
}

/** 构造指针值：目标地址 + 拥有标志（own *T / ref *T / fatal *T 统一） */
value_t *ptr_make_value(vm_t *vm, const type_t *pt,
                        void *target, bool owns);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_PTR_ */
