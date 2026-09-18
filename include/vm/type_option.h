#ifndef _H_CLUX_VM_TYPE_OPTION_
#define _H_CLUX_VM_TYPE_OPTION_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** optional 类型 vtable（薄：eq/ne 无分派——?T 只能与 nil 比较，tag 比较由
 *  编译器发专用指令；clone/assign/dispose 转发 inner + 额外处理 ok） */
extern const vtable_t VTABLE_OPTION;

/* ================================================================ */
/* optional 类型（option_type_t，继承 type_t，?T）                    */
/* ================================================================ */

/**
 * option_type_t 构造对应统一两遍协议 PUSH_OPT / DEFINE_TYPE / LOAD_TYPE /
 * SET_TYPE（设 inner）/ SEAL（与 const/volatile 同族）：
 *   - type_option_push 分配空 option_type（inner=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_option_set_inner 设被包裹类型 T（SET_TYPE 运行期用）；密封后静默
 *     忽略。
 *   - type_option_seal 按 C 对齐规则计算布局（struct Optional_T{bool ok;T
 *     value;}，value 偏移 align_up(1, alignof(T))），按 inner 指针去重
 *     intern，标记 sealed。
 *
 * 内存布局（密封时计算）：
 *   value 字段偏移 voff = align_up(sizeof(bool), inner->align)
 *   size = voff + inner->size；align = max(1, inner->align)
 * SOME 态窄化退化的运行时真相 = 借用引用 data 指向 value 字段（与 is_own
 * 借用字段同构，零拷贝）。
 */
typedef struct option_type_t option_type_t;  /* 定义见 vm/type.h */

/** PUSH_OPT：分配开放 option_type（inner=NULL，不入池）+ 压其 type value */
const type_t *type_option_push(vm_t *vm);

/** SET_TYPE 运行期用：设被包裹类型 T（密封后静默忽略） */
void type_option_set_inner(vm_t *vm, const type_t *t, const type_t *inner);

/** SEAL：按 inner 去重 intern + 计算 C 布局 + 置 sealed。返回该 const type */
const type_t *type_option_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（type_option_push + set_inner + seal）：sema 解析 ?T 用。
 *  不向操作数栈压 type value（嵌套构造场景避免栈布局污染）。 */
const type_t *type_option_intern(vm_t *vm, const type_t *inner);

/** 取 option 类型的 inner（非 option 返回 NULL） */
static inline const type_t *type_option_inner(const type_t *t) {
    return (t && t->kind == TYPE_KIND_OPTION) ? ((const option_type_t *)t)->inner
                                              : NULL;
}

/* ---- option 值块访问器（data 为 struct Optional_T 内存布局） ---- */

/** value 字段偏移（offsetof(value) = align_up(sizeof(bool), alignof(T))） */
static inline size_t option_value_offset(const type_t *t) {
    const type_t *inner = type_option_inner(t);
    size_t a = inner ? inner->align : 1;
    /* align_up(1, a) = max(1, a) */
    return a > 1 ? a : 1;
}

/** 读 ok tag（offset 0，bool） */
static inline bool option_ok(const void *data) {
    return *(const bool *)data;
}

/** value 字段指针（data + offset；借用引用窄化目标） */
static inline void *option_value(const void *data, const type_t *t) {
    return (uint8_t *)data + option_value_offset(t);
}

/**
 * T → ?T 隐式提升（some）：压 ok=true + T 值深拷贝到 value 字段。
 * value_implicit_cast 公共入口特判（目标为 option 且源 == inner）调用。
 * 返回提升后的 ?T value（auto-track）或 error value。
 */
value_t *value_lift_option(vm_t *vm, value_t *v, const type_t *target);

/* ---- 裸数据块深拷贝/资源释放（type_array.c 导出，option clone/assign/
   dispose 复用：ok tag 平凡拷贝 + value 字段按 inner 递归） ---- */

/** 类型化内存块深拷贝：dst ← src 的深拷贝（STR 克隆 string_t、ARRAY/OPTION
 *  递归、标量 memcpy）。shadow 源由调用方先行处理。 */
void value_blit_raw(vm_t *vm, void *dst, const void *src, const type_t *t);

/** 释放类型化内存块持有的资源（STR 释放 string_t、ARRAY/OPTION 递归、标量
 *  无操作）。块内存本身由调用方释放。 */
void value_dispose_raw(vm_t *vm, void *raw, const type_t *t);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_OPTION_ */
