#ifndef _H_PARSER_NODE_STMT_RETURN_
#define _H_PARSER_NODE_STMT_RETURN_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* return; / return expr;（白皮书 §4.11）。
   VALUE 槽为返回表达式，无返回值时为 NULL。 */
#define CLUX_STMT_RETURN_VALUE 0
#define CLUX_STMT_RETURN_CHILDREN_SIZE 1

node_t *create_stmt_return_node(allocator_t *allocator);
/* VALUE 允许为 NULL（return;）。 */
void stmt_return_node_set_value(node_t *self, node_t *value);
node_t *stmt_return_node_get_value(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
