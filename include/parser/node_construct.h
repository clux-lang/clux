#ifndef _H_PARSER_NODE_CONSTRUCT_
#define _H_PARSER_NODE_CONSTRUCT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 构造表达式 .type?{ elem, ... }。
   TYPE 为被构造的类型表达式（匿名构造时为 NULL），其后为逗号分隔的元素，
   数量可变，因此 children 长度 = CLUX_EXPR_CONSTRUCT_FIRST_ELEM + 元素个数。
   语法阶段类型就是普通表达式，按后缀层解析；元素可以是 rest 表达式。 */
#define CLUX_EXPR_CONSTRUCT_TYPE 0
#define CLUX_EXPR_CONSTRUCT_FIRST_ELEM 1

/* 创建时只有 TYPE 槽（无元素）。 */
node_t *create_construct_node(allocator_t *allocator);
/* TYPE 允许为 NULL（匿名构造 .{ ... }）。 */
void construct_node_set_type(node_t *self, node_t *type);
node_t *construct_node_get_type(node_t *self);
void construct_node_add_elem(node_t *self, node_t *elem);
size_t construct_node_get_elem_count(node_t *self);
/* 越界返回 NULL。 */
node_t *construct_node_get_elem(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
