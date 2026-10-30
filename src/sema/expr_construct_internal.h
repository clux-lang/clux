#ifndef _H_CLUX_SEMA_EXPR_CONSTRUCT_INTERNAL_
#define _H_CLUX_SEMA_EXPR_CONSTRUCT_INTERNAL_
/* 内部头：expr_construct.c 拆分后的构造辅助声明（sema 私有）。 */
#include "sema/sema.h"

#ifdef __cplusplus
extern "C" {
#endif


bool option_inner_type_ok(const type_t *inner, const type_t *ft);
bool sema_expand_construct_spread(sema_t *sema, ast_node_t **fields, ast_node_t **fields_last, sema_scope_t *scope, const type_t *target);

/* 守卫：被污染的值（错误已报告）→ 直接返回 void shadow。 */
#define EX_GUARD(sema, v)                                                     \
  do {                                                                        \
    if (is_poison((sema), (v))) return val_void((sema));                       \
  } while (0)

#ifdef __cplusplus
}
#endif
#endif
