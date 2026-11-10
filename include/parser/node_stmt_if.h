#ifndef _H_PARSER_NODE_STMT_IF_
#define _H_PARSER_NODE_STMT_IF_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* if 语句 if (cond) { then } [else { else } | else if ...]（白皮书 §4.4）。
   CONDITION 为条件表达式；THEN 为块语句；
   ELSE 可缺失，存在时为块语句或嵌套的 CLUX_NODE_STMT_IF（else if 链）。 */
#define CLUX_STMT_IF_CONDITION 0
#define CLUX_STMT_IF_THEN 1
#define CLUX_STMT_IF_ELSE 2
#define CLUX_STMT_IF_CHILDREN_SIZE 3

node_t *create_stmt_if_node(allocator_t *allocator);
/* CONDITION / THEN 容错时可为 NULL。 */
void stmt_if_node_set_condition(node_t *self, node_t *condition);
node_t *stmt_if_node_get_condition(node_t *self);
void stmt_if_node_set_then(node_t *self, node_t *then_branch);
node_t *stmt_if_node_get_then(node_t *self);
/* ELSE 允许为 NULL（无 else），或为块语句 / 嵌套 if 语句。 */
void stmt_if_node_set_else(node_t *self, node_t *else_branch);
node_t *stmt_if_node_get_else(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
