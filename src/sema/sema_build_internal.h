#ifndef _H_CLUX_SEMA_BUILD_INTERNAL_
#define _H_CLUX_SEMA_BUILD_INTERNAL_
/* 内部头：stmt_build.c 拆分后的共享结果类型与跨文件声明（sema 私有）。 */
#include "sema/sema.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct build_result {
  bool definitely_returns; /* 该块保证返回（所有路径都 return） */
} build_result_t;

build_result_t build_block(sema_t *sema, ast_block_t *block, sema_scope_t *scope);
void build_local_func(sema_t *sema, ast_node_t *s, sema_scope_t *scope);
void comptime_foreach_splice(sema_t *sema, ast_block_t *block, sema_scope_t *scope);
void comptime_if_splice(sema_t *sema, ast_block_t *block, sema_scope_t *scope);
void desugar_defers(sema_t *sema, ast_block_t *block, sema_scope_t *scope);
void build_name_to_cstr(strslice_t s, char *buf, size_t cap);
bool scope_has_unevaluated_comptime(sema_scope_t *scope);
bool type_has_ct_op(const ast_node_t *type_expr);
bool type_has_nonliteral_array_bound(const ast_node_t *type_expr);
bool type_shadowed_by_local_def(sema_t *sema, sema_scope_t *scope, ast_node_t *type_expr);

/* 守卫：被污染的值（错误已报告）→ 直接返回 void shadow。 */
#define EX_GUARD(sema, v)                                                     \
  do {                                                                        \
    if (is_poison((sema), (v))) return val_void((sema));                       \
  } while (0)

#ifdef __cplusplus
}
#endif
#endif
