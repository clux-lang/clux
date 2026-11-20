#ifndef _H_ENGINE_UNION_
#define _H_ENGINE_UNION_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* tag union 成员：tag 值 = 声明序号，同一时刻只有一个 member 有效。
   （meta 内部存储形态：成员块堆分配，name 为 add_member 自拷的
   自有副本，type 借用类型 owner——经 vtable delete_meta 释放。） */
typedef struct _clux_union_member_t {
  const char *name;
  clux_type_t *type;
} clux_union_member_t;
/* 两阶段构造（白皮书 §3.7，适配递归引用如 union A { f: *A }）：
   create 建未 seal 的空壳（size/align 为 0）；add_member 逐个追加
   成员（name 自拷持有，重名或已 seal 返回 false）；seal 冻结布局：
   tag 为定宽 u32（偏移 0），payload 从对齐处开始
   （align = max(4, 成员最大对齐)），size = round_up(对齐 +
   max(成员 size), align)，此后 set_size/set_align 拒绝。 */
clux_type_t *create_clux_union_type(allocator_t *allocator, const char *name);
bool clux_union_type_add_member(clux_type_t *self,
                                allocator_t *allocator,
                                const char *name,
                                clux_type_t *type);
void clux_union_type_seal(clux_type_t *self);
clux_vtable_t *get_clux_union_vtable(void);
/* 成员表查询：按声明序。 */
size_t clux_union_member_count(clux_type_t *type);
const clux_union_member_t *clux_union_get_member(clux_type_t *type, size_t idx);
const clux_union_member_t *clux_union_find_member(clux_type_t *type,
                                                  const char *name);
/* payload 偏移：tag 之后按类型对齐取整，恒等于类型对齐值。 */
size_t clux_union_payload_offset(clux_type_t *type);
/* 结构判等（成员名+类型按声明序）：把已有 union 类型与原始成员表对比，
   供 context 创建路径做 find-or-dedup，无需先构造临时类型实例。 */
bool clux_union_members_equal(clux_type_t *self,
                              const clux_union_member_t **members,
                              size_t count);
/* 激活成员：切换 tag 为该成员声明序号并清零 payload 区，返回原值；
   未知成员或无效值返回语义化 error。type 须是 CLUX_TYPE_UNION
   （由调用方保证）。 */
clux_value_t *clux_union_set_member(clux_context_t *ctx,
                                    clux_value_t *value,
                                    const char *name);
#ifdef __cplusplus
}
#endif
#endif
