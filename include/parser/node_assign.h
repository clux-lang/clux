#ifndef _H_PARSER_NODE_ASSIGN_
#define _H_PARSER_NODE_ASSIGN_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 赋值表达式 left op value（白皮书 §5.3，右结合最低优先级）：
   左子树是任意无赋值表达式（下标/成员访问/解引用/括号等），
   语法层不区分可赋值目标（set/get 同构），由 sema 检查可赋值性。 */
#define CLUX_EXPR_ASSIGN_CHILDREN_SIZE 3
#define CLUX_EXPR_ASSIGN_LEFT 0
#define CLUX_EXPR_ASSIGN_SYMBOL 1
#define CLUX_EXPR_ASSIGN_VALUE 2

node_t *create_assign_node(allocator_t *allocator);
void assign_node_set_left(node_t *self, node_t *left);
node_t *assign_node_get_left(node_t *self);
void assign_node_set_symbol(node_t *self, node_t *symbol);
node_t *assign_node_get_symbol(node_t *self);
/* VALUE 允许为 NULL（容错：缺右值）。 */
void assign_node_set_value(node_t *self, node_t *value);
node_t *assign_node_get_value(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
