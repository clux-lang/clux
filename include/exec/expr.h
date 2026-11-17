#ifndef _H_EXEC_EXPR_
#define _H_EXEC_EXPR_
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *exec_expr_group(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_binary(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_assign(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_ternary(clux_context_t *ctx, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
