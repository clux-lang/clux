#ifndef _H_PARSER_NODE_STMT_EXPR_
#define _H_PARSER_NODE_STMT_EXPR_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 表达式语句 expr ';'，只有一个子节点。 */
#define CLUX_STMT_EXPR_EXPR 0
#define CLUX_STMT_EXPR_CHILDREN_SIZE 1

node_t *create_stmt_expr_node(allocator_t *allocator);
void stmt_expr_node_set_expr(node_t *self, node_t *expr);
node_t *stmt_expr_node_get_expr(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
