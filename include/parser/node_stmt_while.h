#ifndef _H_PARSER_NODE_STMT_WHILE_
#define _H_PARSER_NODE_STMT_WHILE_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* while 语句 while (cond) { body }（白皮书 §4.6）。
   CONDITION 为条件表达式；BODY 为块语句；容错时均可为 NULL。 */
#define CLUX_STMT_WHILE_CONDITION 0
#define CLUX_STMT_WHILE_BODY 1
#define CLUX_STMT_WHILE_CHILDREN_SIZE 2

node_t *create_stmt_while_node(allocator_t *allocator);
void stmt_while_node_set_condition(node_t *self, node_t *condition);
node_t *stmt_while_node_get_condition(node_t *self);
void stmt_while_node_set_body(node_t *self, node_t *body);
node_t *stmt_while_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
