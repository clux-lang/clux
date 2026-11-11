#ifndef _H_PARSER_NODE_STMT_BITFIELD_
#define _H_PARSER_NODE_STMT_BITFIELD_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 位域语句 <decorator>* <bitfield_decl>（白皮书 §10）。
   BITFIELD_DECL 槽存位域声明；其后按源码顺序为装饰器，数量可变，
   因此 children 长度 = CLUX_STMT_BITFIELD_FIRST_DECORATOR + 装饰器个数。 */
#define CLUX_STMT_BITFIELD_BITFIELD_DECL 0
#define CLUX_STMT_BITFIELD_FIRST_DECORATOR 1

node_t *create_stmt_bitfield_node(allocator_t *allocator);
void stmt_bitfield_node_set_bitfield_decl(node_t *self, node_t *bitfield_decl);
node_t *stmt_bitfield_node_get_bitfield_decl(node_t *self);
/* 装饰器按源码顺序追加。 */
void stmt_bitfield_node_add_decorator(node_t *self, node_t *decorator);
size_t stmt_bitfield_node_get_decorator_count(node_t *self);
/* 越界返回 NULL。 */
node_t *stmt_bitfield_node_get_decorator(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
