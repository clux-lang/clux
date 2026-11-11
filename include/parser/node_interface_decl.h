#ifndef _H_PARSER_NODE_INTERFACE_DECL_
#define _H_PARSER_NODE_INTERFACE_DECL_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 接口声明 interface Name[generics]? { func signature; ... }
   （白皮书 §3.10、§6.9）。装饰器属于外层语句（stmt_interface）。
   KEYWORD 为 `interface` 符号；NAME 为接口名；GENERICS 为泛型参数列表
   （无泛型时为 NULL）；BODY 为成员列表（CLUX_NODE_INTERFACE_MEMBERS）。 */
#define CLUX_EXPR_INTERFACE_DECL_KEYWORD 0
#define CLUX_EXPR_INTERFACE_DECL_NAME 1
#define CLUX_EXPR_INTERFACE_DECL_GENERICS 2
#define CLUX_EXPR_INTERFACE_DECL_BODY 3
#define CLUX_EXPR_INTERFACE_DECL_CHILDREN_SIZE 4

node_t *create_interface_decl_node(allocator_t *allocator);
void interface_decl_node_set_keyword(node_t *self, node_t *keyword);
node_t *interface_decl_node_get_keyword(node_t *self);
void interface_decl_node_set_name(node_t *self, node_t *name);
node_t *interface_decl_node_get_name(node_t *self);
/* GENERICS 允许为 NULL（无泛型）。 */
void interface_decl_node_set_generics(node_t *self, node_t *generics);
node_t *interface_decl_node_get_generics(node_t *self);
/* BODY 为 CLUX_NODE_INTERFACE_MEMBERS；容错时可为 NULL。 */
void interface_decl_node_set_body(node_t *self, node_t *body);
node_t *interface_decl_node_get_body(node_t *self);

/* 接口成员列表，children 为无函数体的 CLUX_NODE_EXPR_FUNC_DECL。 */
node_t *create_interface_members_node(allocator_t *allocator);
void interface_members_node_add_member(node_t *self, node_t *member);
size_t interface_members_node_get_member_count(node_t *self);
/* 越界返回 NULL。 */
node_t *interface_members_node_get_member(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
