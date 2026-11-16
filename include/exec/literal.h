#ifndef _H_EXEC_LITERAL_
#define _H_EXEC_LITERAL_
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *exec_literal_identifier(clux_context_t *ctx, node_t *node);
clux_value_t *exec_literal_numeric(clux_context_t *ctx, node_t *node);
clux_value_t *exec_literal_charactor(clux_context_t *ctx, node_t *node);
clux_value_t *exec_literal_boolean(clux_context_t *ctx, node_t *node);
clux_value_t *exec_literal_string(clux_context_t *ctx, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
