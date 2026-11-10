#ifndef _H_PARSER_NODE_TERNARY_
#define _H_PARSER_NODE_TERNARY_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 三元条件表达式 cond ? then : else（白皮书 §5.3）。 */
#define CLUX_EXPR_TERNARY_CHILDREN_SIZE 3
#define CLUX_EXPR_TERNARY_CONDITION 0
#define CLUX_EXPR_TERNARY_THEN 1
#define CLUX_EXPR_TERNARY_ELSE 2

node_t *create_ternary_node(allocator_t *allocator);
void ternary_node_set_condition(node_t *self, node_t *condition);
node_t *ternary_node_get_condition(node_t *self);
/* THEN/ELSE 允许为 NULL（容错：缺分支）。 */
void ternary_node_set_then(node_t *self, node_t *then);
node_t *ternary_node_get_then(node_t *self);
void ternary_node_set_else(node_t *self, node_t *else_);
node_t *ternary_node_get_else(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
