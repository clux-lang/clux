#ifndef _H_PARSER_NODE_STMT_DO_WHILE_
#define _H_PARSER_NODE_STMT_DO_WHILE_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* do-while 语句 do { body } while (cond);（白皮书 §4.7）。
   BODY 为块语句；CONDITION 为条件表达式；容错时均可为 NULL。
   末尾 ';' 属于语句，区间覆盖到 ';'。 */
#define CLUX_STMT_DO_WHILE_BODY 0
#define CLUX_STMT_DO_WHILE_CONDITION 1
#define CLUX_STMT_DO_WHILE_CHILDREN_SIZE 2

node_t *create_stmt_do_while_node(allocator_t *allocator);
void stmt_do_while_node_set_body(node_t *self, node_t *body);
node_t *stmt_do_while_node_get_body(node_t *self);
void stmt_do_while_node_set_condition(node_t *self, node_t *condition);
node_t *stmt_do_while_node_get_condition(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
