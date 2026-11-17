#ifndef _H_EXEC_EXEC_
#define _H_EXEC_EXEC_
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *exec_node(clux_context_t *ctx, node_t *node);
/* 操作数求值统一消费通道：error 优先传播 + shadow 拒绝 + TDZ 消费校验。
   expr/stmt 的所有操作数读取（右值、条件、实参等）必经此处。 */
clux_value_t *exec_operand(clux_context_t *ctx, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
