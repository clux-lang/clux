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
clux_value_t *exec_stmt_struct(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_block(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_expr(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_return(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_if(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_empty(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_break(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_continue(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_while(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_do_while(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_for(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_foreach(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_switch(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_defer(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_assert(clux_context_t *ctx, node_t *node);
clux_value_t *exec_stmt_static_assert(clux_context_t *ctx, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
