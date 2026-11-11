#ifndef _H_PARSER_NODE_BUILTIN_
#define _H_PARSER_NODE_BUILTIN_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 编译器内建 #name(arg, ...)。
   NAME 为去掉 '#' 的内建名符号；其后为实参，数量可变，
   因此 children 长度 = CLUX_EXPR_BUILTIN_FIRST_ARG + 实参个数。 */
#define CLUX_EXPR_BUILTIN_NAME 0
#define CLUX_EXPR_BUILTIN_FIRST_ARG 1

node_t *create_builtin_node(allocator_t *allocator);
void builtin_node_set_name(node_t *self, node_t *name);
node_t *builtin_node_get_name(node_t *self);
void builtin_node_add_arg(node_t *self, node_t *arg);
size_t builtin_node_get_arg_count(node_t *self);
/* 越界返回 NULL。 */
node_t *builtin_node_get_arg(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
