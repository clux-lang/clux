#ifndef _H_EXEC_EXPR_
#define _H_EXEC_EXPR_
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *exec_expr_group(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_binary(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_subscript(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_slice(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_func_type(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_func_decl(clux_context_t *ctx, node_t *node);
/* 匿名函数值求值：不按名 define，供函数装饰器包装路径使用。 */
clux_value_t *exec_func_decl_value(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_tuple(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_assign(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_ternary(clux_context_t *ctx, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
