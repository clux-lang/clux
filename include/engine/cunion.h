#ifndef _H_ENGINE_CUNION_
#define _H_ENGINE_CUNION_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* cunion 成员：声明序参与结构判等与 get_field 查找，全部共享偏移 0。 */
typedef struct _clux_cunion_member_t {
  const char *name; /* 借用调用方的驻留字符串，类型不拷贝不释放 */
  clux_type_t *type;
} clux_cunion_member_t;
/* 创建具名 C union 类型（无 tag）：size = round_up(max(成员 size), align)，
   align = max(成员 align)，所有成员共享偏移 0（C ABI 布局）。成员表整体
   拷入 meta，成员 name 由调用方保证生命周期（context 路径经 strs 驻留）。 */
clux_type_t *create_clux_cunion_type(allocator_t *allocator,
                                     const char *name,
                                     const clux_cunion_member_t *members,
                                     size_t count);
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
                               const clux_cunion_member_t *members,
                               size_t count);
#ifdef __cplusplus
}
#endif
#endif
