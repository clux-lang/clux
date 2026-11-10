#ifndef _H_PARSER_NODE_STMT_DEFER_
#define _H_PARSER_NODE_STMT_DEFER_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* defer 语句 defer |captures|? { body }（白皮书 §4.12）。
   CAPTURES 为 CLUX_NODE_FUNC_CAPTURES（无捕获时为 NULL）；
   捕获项复用 CLUX_NODE_FUNC_PARAM，NAME 必有，DEFAULT 可缺失；
   BODY 为块语句（容错时可为 NULL）。 */
#define CLUX_STMT_DEFER_CAPTURES 0
#define CLUX_STMT_DEFER_BODY 1
#define CLUX_STMT_DEFER_CHILDREN_SIZE 2

node_t *create_stmt_defer_node(allocator_t *allocator);
/* CAPTURES 允许为 NULL（无捕获）。 */
void stmt_defer_node_set_captures(node_t *self, node_t *captures);
node_t *stmt_defer_node_get_captures(node_t *self);
/* BODY 允许为 NULL（容错：缺 defer 块）。 */
void stmt_defer_node_set_body(node_t *self, node_t *body);
node_t *stmt_defer_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
