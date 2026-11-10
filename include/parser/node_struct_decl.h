#ifndef _H_PARSER_NODE_STRUCT_DECL_
#define _H_PARSER_NODE_STRUCT_DECL_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 结构体声明 struct Name [generics]? [implements ...]? { members }
   （白皮书 §3.5）。装饰器属于外层语句（stmt_struct），不在本节点内。
   KEYWORD 为 `struct` 符号；NAME 为结构体名；
   GENERICS 为泛型参数列表（无泛型时为 NULL）；
   IMPLEMENTS 为接口列表（无 implements 时为 NULL）；
   BODY 为成员列表节点（CLUX_NODE_STRUCT_MEMBERS）。 */
#define CLUX_EXPR_STRUCT_DECL_KEYWORD 0
#define CLUX_EXPR_STRUCT_DECL_NAME 1
#define CLUX_EXPR_STRUCT_DECL_GENERICS 2
#define CLUX_EXPR_STRUCT_DECL_IMPLEMENTS 3
#define CLUX_EXPR_STRUCT_DECL_BODY 4
#define CLUX_EXPR_STRUCT_DECL_CHILDREN_SIZE 5

node_t *create_struct_decl_node(allocator_t *allocator);
void struct_decl_node_set_keyword(node_t *self, node_t *keyword);
node_t *struct_decl_node_get_keyword(node_t *self);
void struct_decl_node_set_name(node_t *self, node_t *name);
node_t *struct_decl_node_get_name(node_t *self);
/* GENERICS 允许为 NULL（无泛型）。 */
void struct_decl_node_set_generics(node_t *self, node_t *generics);
node_t *struct_decl_node_get_generics(node_t *self);
/* IMPLEMENTS 允许为 NULL（无 implements）。 */
void struct_decl_node_set_implements(node_t *self, node_t *implements);
node_t *struct_decl_node_get_implements(node_t *self);
/* BODY 为 CLUX_NODE_STRUCT_MEMBERS；容错时可为 NULL。 */
void struct_decl_node_set_body(node_t *self, node_t *body);
node_t *struct_decl_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
