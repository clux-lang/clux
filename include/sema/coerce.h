#ifndef _H_CLUX_SEMA_COERCE_
#define _H_CLUX_SEMA_COERCE_

#include "parser/ast_node.h"
#include "sema/sema.h"
#include "vm/type.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * **实参/初始化转换的唯一入口**（semantics: one rule, one place）。
 *
 * 把 `*slot` 里的表达式改写成 `want` 需要的表示，返回改写后的 shadow：
 *
 *   1. `want = ?P`、 值不是 `?P`        → 包装成 `.?P{v}`（`nil` 同理）；
 *   2. `want` 与值是不同结构体的指针      → 插入显式 `as`（派生 → 基类）；
 *   3. `want` 是带单参 `__init__` 的 struct → 走初始化派发（`.T{v}`）；
 *   4. 其它：不改写，原样返回（标量隐式转换由 C 负责）。
 *
 * 失败（转换不合法）返回 NULL，且**不**产生诊断——文案与位置由调用方决定。
 * 调用方必须把 `*slot` 重新求值（或用返回的 shadow）后继续。
 */
value_t *sema_coerce(sema_t *sema, ast_node_t **slot, value_t *v,
                     const type_t *want, sema_scope_t *scope);

#ifdef __cplusplus
}
#endif
#endif
