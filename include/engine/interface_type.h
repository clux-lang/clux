#ifndef _H_ENGINE_INTERFACE_TYPE_
#define _H_ENGINE_INTERFACE_TYPE_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 接口成员（白皮书 §3.10、§6.9）：方法签名——名字 + func 类型（无捕获、
   无函数体）。满足是结构的：类型提供全部成员名且签名一致即满足约束，
   不要求显式 implements。name 与 func 类型均借用调用方（context 路径
   名字经 strs 驻留、类型由具名类型表托管），类型不拷贝不释放。 */
typedef struct _clux_interface_member_t {
  const char *name;
  clux_type_t *func;
} clux_interface_member_t;
/* 创建具名 interface 类型（白皮书 §3.10、§6.9）：纯编译期结构约束，
   无运行期对象/虚表——布局无意义（size 0、align 1，与 void 一致）。
   成员表整体拷入 meta。永不 seal（与 void 同族天然不完整）：值创建
   的 seal 门禁由此从根上拒绝 interface 作值类型。 */
clux_type_t *create_clux_interface_type(allocator_t *allocator,
                                        const char *name,
                                        const clux_interface_member_t *members,
                                        size_t count);
clux_vtable_t *get_clux_interface_vtable(void);
/* 成员表查询：按声明序。 */
size_t clux_interface_member_count(clux_type_t *type);
const clux_interface_member_t *clux_interface_get_member(clux_type_t *type,
                                                         size_t idx);
const clux_interface_member_t *clux_interface_find_member(clux_type_t *type,
                                                          const char *name);
/* 结构判等（成员名 + 签名按声明序一一对应即相等，类型名不参与）：
   把已有 interface 类型与原始成员表对比，供 context 创建路径做
   find-or-dedup。非 interface 类型恒为 false。 */
bool clux_interface_members_equal(clux_type_t *self,
                                  const clux_interface_member_t *members,
                                  size_t count);
#ifdef __cplusplus
}
#endif
#endif
