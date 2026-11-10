#ifndef _H_PARSER_NODE_STMT_TYPE_
#define _H_PARSER_NODE_STMT_TYPE_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 类型别名语句 <decorator>* type name [generics]? = expr;（白皮书 §3.11）。
   NAME 为别名；GENERICS 为泛型参数列表（无泛型时为 NULL）；
   VALUE 为别名指向的类型表达式；其后按源码顺序为装饰器，数量可变，
   因此 children 长度 = CLUX_STMT_TYPE_FIRST_DECORATOR + 装饰器个数。 */
#define CLUX_STMT_TYPE_NAME 0
#define CLUX_STMT_TYPE_GENERICS 1
#define CLUX_STMT_TYPE_VALUE 2
#define CLUX_STMT_TYPE_FIRST_DECORATOR 3

node_t *create_stmt_type_node(allocator_t *allocator);
void stmt_type_node_set_name(node_t *self, node_t *name);
node_t *stmt_type_node_get_name(node_t *self);
/* GENERICS 允许为 NULL（无泛型）。 */
void stmt_type_node_set_generics(node_t *self, node_t *generics);
node_t *stmt_type_node_get_generics(node_t *self);
void stmt_type_node_set_value(node_t *self, node_t *value);
node_t *stmt_type_node_get_value(node_t *self);
void stmt_type_node_add_decorator(node_t *self, node_t *decorator);
size_t stmt_type_node_get_decorator_count(node_t *self);
/* 越界返回 NULL。 */
node_t *stmt_type_node_get_decorator(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
