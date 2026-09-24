#ifndef _H_CLUX_VM_TYPE_STRUCT_
#define _H_CLUX_VM_TYPE_STRUCT_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ================================================================ */
/* 结构体类型（struct_type_t，继承 type_t）                           */
/* ================================================================ */

/**
 * struct_type_t 构造对应统一两遍协议 PUSH_STRUCT / DEFINE_TYPE /
 * LOAD_TYPE / DEFINE_FIELD×N / SEAL（与 const/volatile/option/enum 同族）：
 *   - type_struct_push 分配空 struct_type（fields=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_struct_add_field 追加字段（DEFINE_FIELD 运行期用）：名从 strtable
 *     拷贝（vm 拥有），类型引用记录；密封后静默忽略。
 *   - type_struct_seal 拷贝字段表 + 按 C 对齐规则计算布局（offset/size/
 *     align）+ 按 (字段名+类型+顺序) 去重 intern + 置 sealed。
 *
 * 内存布局（密封时计算，C 对齐规则）：
 *   offset_0 = 0；offset_i = align_up(prev_end, field_i.align)
 *   size = align_up(last_end, max_align)；align = max(字段 align)
 * struct value 的 data 是连续内存块（size = type->size），字段按偏移 O(1)
 * 读写。字段类型是布局依赖（须先密封），构造期依赖后序保证。
 */

/** 取结构体类型的字段数量（非 struct 返回 0） */
static inline size_t struct_type_field_count(const type_t *t) {
    return (t && t->kind == TYPE_KIND_STRUCT) ? ((const struct_type_t *)t)->field_count
                                              : 0;
}

/** 取第 i 个字段（越界/非 struct 返回 NULL） */
static inline const struct_field_t *struct_type_field(const type_t *t, size_t i) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
    const struct_type_t *st = (const struct_type_t *)t;
    return (i < st->field_count) ? &st->fields[i] : NULL;
}

/**
 * 按名查字段（sema 构造校验 / 后续成员访问用）：命中返回下标，未找到返回 -1。
 */
int struct_type_find_field(const type_t *t, strslice_t name);

/** PUSH_STRUCT：分配空 struct_type（fields=NULL，不入池）+ 压其 type value */
const type_t *type_struct_push(vm_t *vm);

/** 分配开放 struct type（fields=NULL，不入池，**不压栈**）——union member
 * payload 内部构造用（type_union_add_field 向开放 payload struct 追加字段）。
 * 不向操作数栈压 type value；密封由 type_struct_seal 完成。 */
const type_t *type_struct_alloc_open(vm_t *vm);

/** DEFINE_FIELD 运行期用：追加字段（名拷贝到 vm 堆；密封后静默忽略） */
void type_struct_add_field(vm_t *vm, const type_t *t, strslice_t name,
                           const type_t *ftype);

/** SEAL：拷贝字段表 + C 对齐布局 + 按 (字段名+类型+顺序) 去重 intern + 置 sealed */
const type_t *type_struct_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（push + 全量字段 + seal）：sema 构造 struct 类型用。
 * 不向操作数栈压 type value。 */
const type_t *type_struct_intern(vm_t *vm, const struct_field_t *fields,
                                 size_t count);

/**
 * 结构兼容判断（鸭子类型，m2-design §2）：两 struct 类型字段名 + 字段类型
 * （type_equal，支持嵌套复合）+ 字段顺序完全一致即兼容——布局（offset/size/
 * align）由字段表唯一决定，字段兼容 ⟹ 布局相同。
 *
 * 用于跨具名类型的赋值/判等/隐式转换（struct A 与 struct B 字段一致时可
 * 互赋值）与匿名构造校验（`.{...}` 推断的匿名类型 vs 目标类型）。非 struct
 * 输入返回 false。
 */
bool struct_type_compatible(vm_t *vm, const type_t *a, const type_t *b);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_STRUCT_ */
