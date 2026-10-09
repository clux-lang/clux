#ifndef _H_CLUX_VM_TYPE_SLICE_
#define _H_CLUX_VM_TYPE_SLICE_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 切片类型 vtable（own/ref/fatal 三种所有权修饰，kind 区分） */
extern const vtable_t VTABLE_SLICE_OWN;
extern const vtable_t VTABLE_SLICE_REF;
extern const vtable_t VTABLE_SLICE_FATAL;

/* ================================================================ */
/* 切片类型（slice_type_t，继承 type_t，m4-design §1/§10）           */
/* ================================================================ */

/**
 * slice_type_t 构造对应统一两遍协议 PUSH_SLICE / DEFINE_TYPE / LOAD_TYPE /
 * SET_TYPE（设 elem_type）/ SEAL（与 ptr 同族）：
 *   - type_slice_push 分配空 slice_type（elem_type=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_slice_set_elem 设元素类型 T（SET_TYPE 运行期用）；密封后静默忽略。
 *   - type_slice_seal 按 (elem_type, kind) 去重 intern，标记 sealed。
 *
 * 切片类型的 size/align 恒定（2*指针宽），密封不依赖 elem_type 布局——hoist
 * 中属引用依赖（emit_ref_type，LOAD_TYPE 拉回 elem，不递归），与 ptr 同族。
 */

/** PUSH_SLICE：分配开放 slice_type（elem_type=NULL，不入池）+ 压其 type value。
 *  kind = 所有权修饰（TYPE_KIND_SLICE_OWN/REF/FATAL）。 */
const type_t *type_slice_push(vm_t *vm, type_kind_t kind);

/** SET_TYPE 运行期用：设元素类型 T（密封后静默忽略） */
void type_slice_set_elem(vm_t *vm, const type_t *t, const type_t *elem);

/** SEAL：按 (elem_type, kind) 去重 intern + 置 sealed。返回该 const type */
const type_t *type_slice_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（type_slice_push + set_elem + seal）：sema 解析 own/ref/fatal
 *  []T 用。不向操作数栈压 type value。 */
const type_t *type_slice_intern(vm_t *vm, type_kind_t kind, const type_t *elem);

/** 取切片类型的元素类型 T（非切片返回 NULL） */
static inline const type_t *slice_type_elem(const type_t *t) {
    return (t && (t->kind == TYPE_KIND_SLICE_OWN ||
                  t->kind == TYPE_KIND_SLICE_REF ||
                  t->kind == TYPE_KIND_SLICE_FATAL))
               ? ((const slice_type_t *)t)->elem_type
               : NULL;
}

/* ================================================================ */
/* 切片值布局（m4-design §1.2）                                      */
/* ================================================================ */

/**
 * 切片值（data 块布局）。三种切片的 value data 统一为胖指针：
 *   - ptr：指向元素序列首地址（own 堆分配 / ref 借用 / fatal 将亡值）
 *   - len：元素数量（u64）
 *
 * 所有权由 type kind 区分（运行期零标志，同 M3 §13.2）：
 *   own []T   → 总拥有堆块，dispose 释放（make 产物接管后）
 *   ref []T   → 总借用，dispose 跳过（指向他人堆块 / 数组内联内存）
 *   fatal []T → ptr≠NULL 时拥有（将亡值），转移后 ptr=NULL（接管方拥有）
 *
 * size = sizeof(slice_value_t) = 2 * sizeof(void*)，type_slice_seal 设定。
 */
typedef struct slice_value_t {
    void    *ptr;  /* 指向元素序列首地址 */
    uint64_t len;  /* 元素数量 */
} slice_value_t;

/** 构造切片 value：按 kind 分配/设置胖指针。elem_type 为元素类型。
 *  ptr 指向元素序列首地址，len 为元素数量。返回 auto-tracked value。 */
value_t *slice_make_value(vm_t *vm, const type_t *slice_type,
                          void *ptr, uint64_t len);

/** 读切片 value 的胖指针（非切片 value 返回全零） */
static inline slice_value_t slice_view(const value_t *v) {
    if (!v) return (slice_value_t){ NULL, 0 };
    return *(const slice_value_t *)value_data(v);
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_SLICE_ */
