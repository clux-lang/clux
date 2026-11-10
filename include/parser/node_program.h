#ifndef _H_PARSER_NODE_PROGRAM_
#define _H_PARSER_NODE_PROGRAM_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 程序根节点，children 为按源码顺序排列的语句，数量可变。 */
node_t *create_program_node(allocator_t *allocator);
void program_node_add_stmt(node_t *self, node_t *stmt);
size_t program_node_get_stmt_count(node_t *self);
/* 越界返回 NULL。 */
node_t *program_node_get_stmt(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
