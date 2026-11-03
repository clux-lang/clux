#ifndef _H_CLUX_SEMA_INIT_TDZ_
#define _H_CLUX_SEMA_INIT_TDZ_

#include "parser/ast_func_def.h"
#include "sema/sema.h"
#include "vm/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * `__init__` / `__init_copy__` 的**字段 TDZ + 完整性**检查。
 *
 * self 指向"整体分配、字段只置零"的存储：字段在赋值前不是有效的 T。
 * 约束：
 *   1. 任一 `return` 之前，self 的所有字段都已被赋值
 *      （`self.* = v` 整体赋值，或逐字段赋值）；
 *   2. 字段首次赋值之前不得读取（读 / 方法调用 / 复合赋值读改写）。
 *
 * 保守实现：分支汇合取交集，循环体不保证执行（循环里赋的值不算"肯定"）。
 */
void sema_check_init_body(sema_t *sema, const type_t *owner,
                          ast_func_def_t *fn);

#ifdef __cplusplus
}
#endif
#endif
