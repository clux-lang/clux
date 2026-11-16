#ifndef _H_EXEC_EXEC_
#define _H_EXEC_EXEC_
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *exec_node(clux_context_t *ctx, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
