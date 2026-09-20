#ifndef _H_CLUX_VM_TYPE_ENUM_
#define _H_CLUX_VM_TYPE_ENUM_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ================================================================ */
/* 枚举类型（enum_type_t，继承 type_t）                               */
/* ================================================================ */

/**
 * enum_type_t 构造对应统一两遍协议 PUSH_ENUM / DEFINE_TYPE / LOAD_TYPE /
 * SET_TYPE（设 underlying）/ ENUM_VARIANT×N / SEAL（与 const/volatile/
 * option 同族）：
 *   - type_enum_push 分配空 enum_type（underlying=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_enum_set_underlying 设底层整型（SET_TYPE 运行期用）；密封后静默
 *     忽略。底层必须为 TYPE_KIND_INT（sema 已校验，运行期不重复检查）。
 *   - type_enum_add_variant 追加 variant（ENUM_VARIANT 运行期用）：名从
 *     strtable 拷贝（vm 拥有），值按 underlying->size 截断存储。密封后静默
 *     忽略。
 *   - type_enum_seal 拷贝 variant 表 + 布局（size/align = underlying）+
 *     按 (underlying, variant 表内容) 去重 intern + 置 sealed。
 *
 * 内存布局（密封时计算）：enum value 的 data 是底层宽度的整数值块——
 *   size  = underlying->size；align = underlying->align
 * 严格类型分离：enum 与底层互不隐式转换；显式 cast 仅 enum → 声明底层
 * 类型；判等仅同 enum 实例之间。
 */

/** 取枚举类型的底层类型（非枚举返回 NULL） */
static inline const type_t *enum_type_underlying(const type_t *t) {
    return (t && t->kind == TYPE_KIND_ENUM) ? ((const enum_type_t *)t)->underlying
                                            : NULL;
}

/** 取枚举类型的 variant 数量（非枚举返回 0） */
static inline size_t enum_type_variant_count(const type_t *t) {
    return (t && t->kind == TYPE_KIND_ENUM) ? ((const enum_type_t *)t)->variant_count
                                            : 0;
}

/** 取第 i 个 variant（越界/非枚举返回 NULL） */
static inline const enum_variant_t *enum_type_variant(const type_t *t, size_t i) {
    if (!t || t->kind != TYPE_KIND_ENUM) return NULL;
    const enum_type_t *et = (const enum_type_t *)t;
    return (i < et->variant_count) ? &et->variants[i] : NULL;
}

/**
 * 按名查 variant（sema AST_ENUM_REF 折叠用）：命中返回下标，未找到返回 -1。
 */
int enum_type_find_variant(const type_t *t, strslice_t name);

/** PUSH_ENUM：分配开放 enum_type（underlying=NULL，不入池）+ 压其 type value */
const type_t *type_enum_push(vm_t *vm);

/** SET_TYPE 运行期用：设底层整型（密封后静默忽略） */
void type_enum_set_underlying(vm_t *vm, const type_t *t, const type_t *underlying);

/** ENUM_VARIANT 运行期用：追加 variant（名拷贝到 vm 堆，值按底层宽度截断） */
void type_enum_add_variant(vm_t *vm, const type_t *t, strslice_t name, int64_t value);

/** SEAL：拷贝 variant 表 + 布局 + 按 (underlying, variants) 去重 intern + 置 sealed */
const type_t *type_enum_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（push + set_underlying + 全量 variant + seal）：sema 构造 enum
 *  类型用。不向操作数栈压 type value。 */
const type_t *type_enum_intern(vm_t *vm, const type_t *underlying,
                               const enum_variant_t *variants, size_t count);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_ENUM_ */
