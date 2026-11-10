#ifndef _H_PARSER_NODE_STMT_SWITCH_
#define _H_PARSER_NODE_STMT_SWITCH_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* switch 语句 switch (value) { branch... }（白皮书 §4.5）。
   VALUE 为被匹配表达式；BODY 为分支列表（CLUX_NODE_SWITCH_BRANCHES）；
   容错时均可为 NULL。 */
#define CLUX_STMT_SWITCH_VALUE 0
#define CLUX_STMT_SWITCH_BODY 1
#define CLUX_STMT_SWITCH_CHILDREN_SIZE 2

node_t *create_stmt_switch_node(allocator_t *allocator);
void stmt_switch_node_set_value(node_t *self, node_t *value);
node_t *stmt_switch_node_get_value(node_t *self);
void stmt_switch_node_set_body(node_t *self, node_t *body);
node_t *stmt_switch_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
