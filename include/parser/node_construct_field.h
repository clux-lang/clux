#ifndef _H_PARSER_NODE_CONSTRUCT_FIELD_
#define _H_PARSER_NODE_CONSTRUCT_FIELD_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 具名构造字段 .name = value（白皮书 §5.10）。
   NAME 为字段名标识符，VALUE 为初始化表达式。 */
#define CLUX_EXPR_CONSTRUCT_FIELD_NAME 0
#define CLUX_EXPR_CONSTRUCT_FIELD_VALUE 1

node_t *create_construct_field_node(allocator_t *allocator);
void construct_field_node_set_name(node_t *self, node_t *name);
node_t *construct_field_node_get_name(node_t *self);
void construct_field_node_set_value(node_t *self, node_t *value);
node_t *construct_field_node_get_value(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
