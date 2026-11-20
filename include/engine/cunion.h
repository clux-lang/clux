#ifndef _H_ENGINE_CUNION_
#define _H_ENGINE_CUNION_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* cunion 成员：声明序参与结构判等与 field 查找，全部共享偏移 0。
   （meta 内部存储形态：成员块堆分配，name 为 add_member 自拷的
   自有副本，type 借用类型 owner——经 vtable delete_meta 释放。） */
typedef struct _clux_cunion_member_t {
  const char *name;
  clux_type_t *type;
} clux_cunion_member_t;
/* 两阶段构造（与 tag union 同构，适配递归引用）：create 建未 seal 的
   空壳（size/align 为 0）；add_member 逐个追加成员（name 自拷持有，
   重名或已 seal 返回 false）；seal 冻结布局（C ABI）：
   align = max(成员 align)（下限 1），
   size = round_up(max(成员 size), align)。 */
clux_type_t *create_clux_cunion_type(allocator_t *allocator, const char *name);
bool clux_cunion_type_add_member(clux_type_t *self,
                                 allocator_t *allocator,
                                 const char *name,
                                 clux_type_t *type);
void clux_cunion_type_seal(clux_type_t *self);
clux_vtable_t *get_clux_cunion_vtable(void);
/* 成员表查询：按声明序。 */
size_t clux_cunion_member_count(clux_type_t *type);
const clux_cunion_member_t *clux_cunion_get_member(clux_type_t *type,
                                                   size_t idx);
const clux_cunion_member_t *clux_cunion_find_member(clux_type_t *type,
                                                    const char *name);
/* 结构判等（成员名+类型按声明序）：把已有 cunion 类型与原始成员表对比，
   供 context 创建路径做 find-or-dedup，无需先构造临时类型实例。 */
bool clux_cunion_members_equal(clux_type_t *self,
                               const clux_cunion_member_t **members,
                               size_t count);
#ifdef __cplusplus
}
#endif
#endif
