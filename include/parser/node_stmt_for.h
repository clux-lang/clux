#ifndef _H_PARSER_NODE_STMT_FOR_
#define _H_PARSER_NODE_STMT_FOR_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* for 语句 for (init; cond; update) { body }（白皮书 §4.8）。
   INIT 为 CLUX_NODE_STMT_VAR 或初始化表达式（可缺失）；
   CONDITION / UPDATE 为表达式（可缺失）；BODY 为块语句。
   四个槽容错时均可为 NULL。 */
#define CLUX_STMT_FOR_INIT 0
#define CLUX_STMT_FOR_CONDITION 1
#define CLUX_STMT_FOR_UPDATE 2
#define CLUX_STMT_FOR_BODY 3
#define CLUX_STMT_FOR_CHILDREN_SIZE 4

node_t *create_stmt_for_node(allocator_t *allocator);
/* INIT 允许为 NULL（省略初始化）。 */
void stmt_for_node_set_init(node_t *self, node_t *init);
node_t *stmt_for_node_get_init(node_t *self);
/* CONDITION 允许为 NULL（省略条件）。 */
void stmt_for_node_set_condition(node_t *self, node_t *condition);
node_t *stmt_for_node_get_condition(node_t *self);
/* UPDATE 允许为 NULL（省略更新）。 */
void stmt_for_node_set_update(node_t *self, node_t *update);
node_t *stmt_for_node_get_update(node_t *self);
/* BODY 容错时可为 NULL。 */
void stmt_for_node_set_body(node_t *self, node_t *body);
node_t *stmt_for_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
