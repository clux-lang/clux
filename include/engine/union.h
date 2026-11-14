#ifndef _H_ENGINE_UNION_
#define _H_ENGINE_UNION_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* tag union 成员：tag 值 = 声明序号，同一时刻只有一个 member 有效。 */
typedef struct _clux_union_member_t {
  const char *name; /* 借用调用方的驻留字符串，类型不拷贝不释放 */
  clux_type_t *type;
} clux_union_member_t;
/* 创建具名 tag union 类型（白皮书 §3.7）：tag 为定宽 u32（偏移 0），
   payload 从类型对齐处开始（align = max(4, 成员最大对齐)，即
   round_up(sizeof(u32), align)），size = round_up(payload 偏移 + max(成员
   size), align)。成员表整体拷入 meta，成员 name 由调用方保证生命周期
   （context 路径经 strs 驻留）。 */
clux_type_t *create_clux_union_type(allocator_t *allocator,
                                    const char *name,
                                    const clux_union_member_t *members,
                                    size_t count);
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
                              const clux_union_member_t *members,
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
