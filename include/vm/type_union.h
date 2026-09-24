#ifndef _H_CLUX_VM_TYPE_UNION_
#define _H_CLUX_VM_TYPE_UNION_

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
/* tag union 类型（union_type_t，继承 type_t）                        */
/* ================================================================ */

/**
 * union_type_t 构造对应统一两遍协议 PUSH_UNION / DEFINE_TYPE /
 * LOAD_TYPE / UNION_MEMBER×N / SEAL（与 struct/tuple 同族）：
 *   - type_union_push 分配空 union_type（members=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_union_add_member 追加 member（UNION_MEMBER 运行期用）：名从
 *     strtable 拷贝（vm 拥有），tag 编号 = 追加序；payload 字段表经
 *     type_union_add_field（DEFINE_FIELD 复用）追加到当前 member 的
 *     开放 payload struct。密封后静默忽略。
 *   - type_union_seal 拷贝 member 表 + 构建各 member payload struct +
 *     计算布局（tag 宽度自适应 + payload 联合体偏移） + 按 (member 表
 *     内容) 去重 intern + 置 sealed。
 *
 * 内存布局（密封时计算）：
 *   tag_size：member_count <= UINT8_MAX → 1；<= UINT16_MAX → 2；
 *             <= UINT32_MAX → 4；否则 8
 *   payload_offset = align_up(tag_size, max payload align)（纯 tag member
 *     union 无 payload：size = tag_size）
 *   size = payload_offset + max(member payload size)（C 对齐收尾）
 * union value 的 data 首部存 tag 整数值（union_read_tag / union_store_tag
 * 按 tag_size 读写），payload 联合体按当前 tag 的 member 解释。
 *
 * 字段访问（FIELD_GET/FIELD_SET）：按字段名反查所属 member（sema 已校验
 * 字段名全局唯一）→ 读取 data 首部 tag → tag 与 member 不符 → error
 * （对齐数组越界的运行期硬错误）；相符 → payload_struct 内偏移 + 绝对
 * 偏移 = payload_offset + member 内偏移。
 *
 * tag 判定（`x is Member`）：运行期比较 tag 整数与编译期 member 的 tag 值。
 */

/** 取 union 类型的 member 数量（非 union 返回 0） */
static inline size_t union_type_member_count(const type_t *t) {
    return (t && t->kind == TYPE_KIND_UNION) ? ((const union_type_t *)t)->member_count
                                             : 0;
}

/** 取第 i 个 member（越界/非 union 返回 NULL） */
static inline const union_member_t *union_type_member(const type_t *t, size_t i) {
    if (!t || t->kind != TYPE_KIND_UNION) return NULL;
    const union_type_t *ut = (const union_type_t *)t;
    return (i < ut->member_count) ? &ut->members[i] : NULL;
}

/** 取 union 的 payload 联合体起始偏移（非 union 返回 0） */
static inline size_t union_type_payload_offset(const type_t *t) {
    return (t && t->kind == TYPE_KIND_UNION) ? ((const union_type_t *)t)->payload_offset
                                             : 0;
}

/** 取 union 的 tag 宽度（非 union 返回 0） */
static inline size_t union_type_tag_size(const type_t *t) {
    return (t && t->kind == TYPE_KIND_UNION) ? ((const union_type_t *)t)->tag_size
                                             : 0;
}

/**
 * 按名查 member（sema is 校验 / 字段反查用）：命中返回下标，未找到返回 -1。
 */
int union_type_find_member(const type_t *t, strslice_t name);

/**
 * 按名查 member payload 字段（FIELD_GET/SET 运行期反查所属 member）：
 * 命中返回 { member 下标, payload_struct 内字段下标 }；未找到两值均 -1。
 * 字段名须全局唯一（sema 已校验——member payload 字段表是联合体，不同
 * member 的同名字段无法静态区分归属）。
 */
void union_type_find_field(const type_t *t, strslice_t name,
                           int *member_idx, int *field_idx);

/** 读 union value 的 tag 整数值（按 tag_size 宽度；非 union 返回 0） */
uint64_t union_read_tag(const value_t *v);

/** 从裸 data 块读 tag 整数值（value_blit_raw/value_dispose_raw 内部用：
 * 按 tag 宽度读 data 首部；ts 为 union_type_tag_size(t)） */
static inline uint64_t union_read_tag_raw(const void *data, size_t ts) {
    switch (ts) {
    case 1: return (uint64_t)*(const uint8_t  *)data;
    case 2: return (uint64_t)*(const uint16_t *)data;
    case 4: return (uint64_t)*(const uint32_t *)data;
    default: return *(const uint64_t *)data;
    }
}

/** 向裸 data 块写 tag 整数值（op_construct union 分支内部用，按 tag 宽度截断） */
static inline void union_store_tag_raw(void *data, size_t ts, uint64_t tag) {
    switch (ts) {
    case 1: *(uint8_t  *)data = (uint8_t)tag;  break;
    case 2: *(uint16_t *)data = (uint16_t)tag; break;
    case 4: *(uint32_t *)data = (uint32_t)tag; break;
    default: *(uint64_t *)data = tag;          break;
    }
}

/** PUSH_UNION：分配空 union_type（members=NULL，不入池）+ 压其 type value */
const type_t *type_union_push(vm_t *vm);

/** UNION_MEMBER 运行期用：追加 member（名拷贝到 vm 堆，tag = 追加序；
 *  payload 字段表经 type_union_add_field 追加。密封后静默忽略） */
void type_union_add_member(vm_t *vm, const type_t *t, strslice_t name);

/** DEFINE_FIELD 运行期用（union 分支）：向当前（最后追加）member 的开放
 *  payload struct 追加字段（名拷贝到 vm 堆）。密封后静默忽略。 */
void type_union_add_field(vm_t *vm, const type_t *t, strslice_t name,
                          const type_t *ftype);

/** SEAL：拷贝 member 表 + 构建 payload struct + 布局 + 去重 intern + 置 sealed */
const type_t *type_union_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（push + 全量 member + seal）：sema 构造 union 类型用。
 * 不向操作数栈压 type value。 */
const type_t *type_union_intern(vm_t *vm, const union_member_t *members,
                                size_t count);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_UNION_ */
