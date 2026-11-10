#ifndef _H_PARSER_NODE_TUPLE_
#define _H_PARSER_NODE_TUPLE_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 元组类型 <T, U, ...>。元素数量可变，可含 rest 表达式，也可为空。
   元素从下标 0 起，children 长度 = 元素个数。 */
node_t *create_tuple_node(allocator_t *allocator);
void tuple_node_add_elem(node_t *self, node_t *elem);
size_t tuple_node_get_elem_count(node_t *self);
/* 越界返回 NULL。 */
node_t *tuple_node_get_elem(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
