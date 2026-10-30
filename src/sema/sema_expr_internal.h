#ifndef _H_CLUX_SEMA_EXPR_INTERNAL_
#define _H_CLUX_SEMA_EXPR_INTERNAL_
/* 内部头：expr.c 拆分后的跨文件声明（sema 私有，不属于公共 API）。 */
#include "sema/sema.h"
#include "vm/type_struct.h"

#ifdef __cplusplus
extern "C" {
#endif


value_t *ex_addr(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_array(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_call(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_const(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_construct(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_deref(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_float_lit(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_func_def(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_func_ref(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_ident(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_index(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_int_lit(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_make(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_member(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_move(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_scope_ref(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_sizeof_alignof(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_slice(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_slice_type(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_ternary(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_type_expr(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_type_ref(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_typeof(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_unary(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_unwrap(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
value_t *ex_volatile(sema_t *sema, ast_node_t **node, sema_scope_t *scope);
const struct_type_t *find_runtime_attr_owner(const type_t *t, const char *name);
bool is_poison(sema_t *sema, const value_t *v);
void op_text(const token_t *op, char *buf, size_t cap);
void op_type_name(value_t *v, char *buf, size_t cap);
bool reflect_member_call(sema_t *sema, ast_node_t **node, sema_scope_t *scope, value_t **out);
const type_t *resolve_cross_module_struct(sema_t *sema, ast_scope_ref_t *er, sema_scope_t *scope);
value_t *val_void(sema_t *sema);

/* 守卫：被污染的值（错误已报告）→ 直接返回 void shadow。 */
#define EX_GUARD(sema, v)                                                     \
  do {                                                                        \
    if (is_poison((sema), (v))) return val_void((sema));                       \
  } while (0)

#ifdef __cplusplus
}
#endif
#endif
