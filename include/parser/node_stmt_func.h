#ifndef _H_PARSER_NODE_STMT_FUNC_
#define _H_PARSER_NODE_STMT_FUNC_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 函数语句 <decorator>* <func_decl>（白皮书 §3.3）。
   FUNC_DECL 槽存函数声明；其后按源码顺序为装饰器，数量可变，
   因此 children 长度 = CLUX_STMT_FUNC_FIRST_DECORATOR + 装饰器个数。 */
#define CLUX_STMT_FUNC_FUNC_DECL 0
#define CLUX_STMT_FUNC_FIRST_DECORATOR 1

node_t *create_stmt_func_node(allocator_t *allocator);
void stmt_func_node_set_func_decl(node_t *self, node_t *func_decl);
node_t *stmt_func_node_get_func_decl(node_t *self);
/* 装饰器按源码顺序追加。 */
void stmt_func_node_add_decorator(node_t *self, node_t *decorator);
size_t stmt_func_node_get_decorator_count(node_t *self);
/* 越界返回 NULL。 */
node_t *stmt_func_node_get_decorator(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
