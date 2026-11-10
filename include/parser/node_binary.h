#ifndef _H_PARSER_NODE_BINARY_
#define _H_PARSER_NODE_BINARY_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 二元表达式节点（白皮书 §5.3）。
   一元前缀/后缀运算符复用同一形态：前缀 LEFT 为 NULL，后缀 RIGHT 为 NULL，
   靠缺失的一侧区分三种用法。 */
#define CLUX_EXPR_BINARY_CHILDREN_SIZE 3
#define CLUX_EXPR_BINARY_LEFT 0
#define CLUX_EXPR_BINARY_SYMBOL 1
#define CLUX_EXPR_BINARY_RIGHT 2

/* 创建二元/一元/后缀节点。三者共用同一节点形态，槽位全部预置为 NULL，
   由调用方按语义填入对应侧。 */
node_t *create_binary_node(allocator_t *allocator);
node_t *create_unary_node(allocator_t *allocator);
node_t *create_postfix_node(allocator_t *allocator);

/* 槽位读写。LEFT/RIGHT 允许为 NULL（一元形态缺一侧）；SYMBOL 必有。 */
void binary_node_set_left(node_t *self, node_t *left);
node_t *binary_node_get_left(node_t *self);
void binary_node_set_symbol(node_t *self, node_t *symbol);
node_t *binary_node_get_symbol(node_t *self);
void binary_node_set_right(node_t *self, node_t *right);
node_t *binary_node_get_right(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
