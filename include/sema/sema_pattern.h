#ifndef _H_CLUX_SEMA_SEMA_PATTERN_
#define _H_CLUX_SEMA_SEMA_PATTERN_

#include "parser/ast_node.h"
#include "sema/sema.h"
#include "vm/type.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * M7 `extends` PATTERN 匹配（docs/m7-design.md §27–§29）：
 * 把具体类型 t 与 PATTERN 树递归对位。只判断，不绑定/不捕获。
 * 失败返回 false（调用方负责诊断；本函数不产诊断）。
 */
bool sema_pattern_match(sema_t *sema, ast_node_t *pat, const type_t *t);

#ifdef __cplusplus
}
#endif
#endif
