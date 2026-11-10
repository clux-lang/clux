#ifndef _H_PARSER_NODE_GROUP_
#define _H_PARSER_NODE_GROUP_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 括号表达式 ( expr )：只有一个子节点，用于改变结合/优先级。
   语法层不校验括号内是值还是语句，是否为合法值推迟到语义阶段。 */
#define CLUX_EXPR_GROUP_EXPR 0

node_t *create_group_node(allocator_t *allocator);
/* EXPR 允许为 NULL（容错：括号内缺表达式）。 */
void group_node_set_expr(node_t *self, node_t *expr);
node_t *group_node_get_expr(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
