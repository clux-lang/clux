#ifndef _H_EXEC_STMT_
#define _H_EXEC_STMT_
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *exec_stmt_var(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_type(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_func(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_union(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_enum(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_interface(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_block(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_expr(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_return(clux_context_t *ctx, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
