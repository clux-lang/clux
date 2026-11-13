#ifndef _H_ENGINE_QUALIFIER_
#define _H_ENGINE_QUALIFIER_
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* const/volatile 限定复合类型：挂在 base 的 children 表中按 kind 分桶
   interning（与 ptr 同模式）。size/align 承接 base；vtable 是静态委托：
   每个操作基于左值创建 base 类型的 view（内存共享）后走标准入口，
   嵌套限定经递归 view 自然解开。限定语义只在写入点（通用 assign）生效：
   const 阻断写入，volatile 阻断优化但不阻断读写。 */
clux_type_t *clux_type_get_qualified_type(clux_type_t *base,
                                          clux_type_kind_t qualifier);
/* 非 const/volatile 类型返回 NULL，否则返回被限定的 base。 */
clux_type_t *clux_type_get_qualified_base(clux_type_t *type);
/* 剥除全部 const/volatile 限定层，返回最底层的非限定类型。 */
clux_type_t *clux_type_unwrap(clux_type_t *type);
/* unwrap 后的 kind（NULL 安全：返回 CLUX_TYPE_ERROR）。 */
clux_type_kind_t clux_type_kind_unwrap(clux_type_t *type);
/* 限定链上是否出现 const。 */
bool clux_type_is_const(clux_type_t *type);
#ifdef __cplusplus
}
#endif
#endif
