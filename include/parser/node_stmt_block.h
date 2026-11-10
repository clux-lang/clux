#ifndef _H_PARSER_NODE_STMT_BLOCK_
#define _H_PARSER_NODE_STMT_BLOCK_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 块语句 { stmt* }，children 为语句，数量可变。 */
node_t *create_stmt_block_node(allocator_t *allocator);
void stmt_block_node_add_stmt(node_t *self, node_t *stmt);
size_t stmt_block_node_get_stmt_count(node_t *self);
/* 越界返回 NULL。 */
node_t *stmt_block_node_get_stmt(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
