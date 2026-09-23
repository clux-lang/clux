#ifndef _H_CLUX_VM_TYPE_TUPLE_
#define _H_CLUX_VM_TYPE_TUPLE_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ================================================================ */
/* 元组类型（tuple_type_t，继承 type_t，<T1,T2>）                      */
/* ================================================================ */

/**
 * tuple_type_t 构造对应统一两遍协议 PUSH_TUPLE / DEFINE_TYPE /
 * LOAD_TYPE / APPEND_ELEM×N / SEAL（与 const/volatile/option/enum/struct
 * 同族）：
 *   - type_tuple_push 分配空 tuple_type（elems=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_tuple_add_elem 追加元素（APPEND_ELEM 运行期用）：类型引用记录，
 *     元素匿名（无名字）；密封后静默忽略。
 *   - type_tuple_seal 拷贝元素表 + 按 C 对齐规则计算布局（offset/size/
 *     align）+ 按（元素类型+顺序）去重 intern + 置 sealed。
 *
 * 内存布局（密封时计算，C 对齐规则，与 struct 同款）：
 *   offset_0 = 0；offset_i = align_up(prev_end, elem_i.align)
 *   size = align_up(last_end, max_align)；align = max(元素 align)
 * tuple value 的 data 是连续内存块（size = type->size），元素按偏移 O(1)
 * 读写。元素类型是布局依赖（须先密封），构造期依赖后序保证。
 */

/** 元组类型 vtable（eq/ne 按元素递归；implicit_cast Tuple↔Array 布局兼容
 *  互转；clone/assign/dispose 按元素递归） */
extern const vtable_t VTABLE_TUPLE;

/** 取元组类型的元素数量（非 tuple 返回 0） */
static inline size_t tuple_type_elem_count(const type_t *t) {
    return (t && t->kind == TYPE_KIND_TUPLE) ? ((const tuple_type_t *)t)->elem_count
                                              : 0;
}

/** 取第 i 个元素（越界/非 tuple 返回 NULL） */
static inline const tuple_elem_t *tuple_type_elem(const type_t *t, size_t i) {
    if (!t || t->kind != TYPE_KIND_TUPLE) return NULL;
    const tuple_type_t *tt = (const tuple_type_t *)t;
    return (i < tt->elem_count) ? &tt->elems[i] : NULL;
}

/** PUSH_TUPLE：分配空 tuple_type（elems=NULL，不入池）+ 压其 type value */
const type_t *type_tuple_push(vm_t *vm);

/** APPEND_ELEM 运行期用：追加元素（类型引用记录；密封后静默忽略） */
void type_tuple_add_elem(vm_t *vm, const type_t *t, const type_t *etype);

/** SEAL：拷贝元素表 + C 对齐布局 + 按（元素类型+顺序）去重 intern + 置 sealed */
const type_t *type_tuple_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（push + 全量元素 + seal）：sema 构造 tuple 类型用。
 * 不向操作数栈压 type value。 */
const type_t *type_tuple_intern(vm_t *vm, const tuple_elem_t *elems,
                                size_t count);

/**
 * 元组兼容判断（鸭子类型，m2-design §3）：两 tuple 类型元素类型（type_equal，
 * 支持嵌套复合）+ 元素顺序完全一致即兼容——布局（offset/size/align）由元素
 * 表唯一决定，元素兼容 ⟹ 布局相同。非 tuple 输入返回 false。
 */
bool tuple_type_compatible(vm_t *vm, const type_t *a, const type_t *b);

/**
 * Tuple↔Array 布局兼容判断（双向，m2-design §3 元组↔数组匿名互转）：
 * 元素逐个 type_equal + 数量一致 ⟹ size/align 由元素表唯一决定相同。
 * 非 tuple/array 组合返回 false。array 侧 implicit_cast / assign 复用。
 */
bool tuple_array_layout_compatible(vm_t *vm, const type_t *a, const type_t *b);

/* ---- 运行期 tuple value 只读访问（供调试/格式化遍历，如 printf %v） ---- */

/** 前置声明：避免为仅消费指针的访问器引入整个 value.h */
typedef struct value_t value_t;

/** 返回 tuple value 的元素个数（借用/普通一致，从类型取；非 tuple value 返回 0） */
size_t value_tuple_count(const value_t *v);

/**
 * 返回第 i 个元素的借用引用（data 指向块内偏移业务内存，type = 元素类型），
 * 越界返回 NULL。供调试/格式化遍历（如 printf %v）递归使用；借用值 auto-track
 * 到 vm->current_scope。
 */
value_t *value_tuple_at(vm_t *vm, const value_t *v, size_t i);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_TUPLE_ */
