#ifndef _H_CLUX_VM_TYPE_CUNION_
#define _H_CLUX_VM_TYPE_CUNION_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- 前置声明：避免为仅消费指针的访问器引入整个 value.h ---- */
typedef struct value_t value_t;

/* ================================================================ */
/* C 语义 union 类型（cunion_type_t，继承 type_t）                    */
/* ================================================================ */

/**
 * cunion_type_t 构造对应统一两遍协议 PUSH_CUNION / DEFINE_TYPE /
 * LOAD_TYPE / DEFINE_FIELD×N / SEAL（与 struct 同族，无 tag）：
 *   - type_cunion_push 分配空 cunion_type（members=NULL，不入池）+ 压其
 *     type value，返回该 type（外部只持有 type_t*）。
 *   - type_cunion_set_member_type 追加 member 类型（DEFINE_FIELD 复用，语义
 *     = 设最后追加的 member 的类型）。member 名由 DEFINE_FIELD 指令经
 *     type_cunion_add_member 写入。密封后静默忽略。
 *   - type_cunion_seal 拷贝 member 表 + 计算布局（size = max(member size)、
 *     align = max(member align)，C 对齐；空列表 size=1）+ 按 (member 表内容)
 *     去重 intern + 置 sealed。
 *
 * 内存布局（密封时计算）：
 *   size = align_up(max(member size), max_align)；align = max(member align)
 *   （空 member 列表 → size = 1，保证 data 块可分配）
 * 所有 member 共享同一内存块：offset 全为 0（C union 语义），无 tag。
 * cunion value 的 data 即联合体本身。
 *
 * 字段访问（FIELD_GET/FIELD_SET）：按字段名 = member 名反查 → 绝对偏移 = 0
 * （无 tag 校验，C union 语义）。
 */

/** 取 cunion 类型的 member 数量（非 cunion 返回 0） */
static inline size_t cunion_type_member_count(const type_t *t) {
    return (t && t->kind == TYPE_KIND_CUNION)
               ? ((const cunion_type_t *)t)->member_count
               : 0;
}

/** 取第 i 个 member（越界/非 cunion 返回 NULL） */
static inline const cunion_member_t *cunion_type_member(const type_t *t, size_t i) {
    if (!t || t->kind != TYPE_KIND_CUNION) return NULL;
    const cunion_type_t *ct = (const cunion_type_t *)t;
    return (i < ct->member_count) ? &ct->members[i] : NULL;
}

/**
 * 按名查 member（sema 字段反查用）：命中返回下标，未找到返回 -1。
 */
int cunion_type_find_member(const type_t *t, strslice_t name);

/** PUSH_CUNION：分配空 cunion_type（members=NULL，不入池）+ 压其 type value */
const type_t *type_cunion_push(vm_t *vm);

/** DEFINE_FIELD 运行期用（cunion 分支）：追加 member（名拷贝到 vm 堆）。
 *  密封后静默忽略。 */
void type_cunion_add_member(vm_t *vm, const type_t *t, strslice_t name);

/** DEFINE_FIELD 运行期用（cunion 分支）：设置最后追加的 member 的类型。
 *  密封后静默忽略。 */
void type_cunion_set_member_type(vm_t *vm, const type_t *t, const type_t *ftype);

/** SEAL：拷贝 member 表 + 布局 + 去重 intern + 置 sealed */
const type_t *type_cunion_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（push + 全量 member + seal）：sema 构造 cunion 类型用。
 * 不向操作数栈压 type value。 */
const type_t *type_cunion_intern(vm_t *vm, const cunion_member_t *members,
                                 size_t count);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_CUNION_ */
