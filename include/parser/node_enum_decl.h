#ifndef _H_PARSER_NODE_ENUM_DECL_
#define _H_PARSER_NODE_ENUM_DECL_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 枚举声明 enum Name: Underlying { Variant = value, ... }（白皮书 §3.6）。
   装饰器属于外层语句（stmt_enum），不在本节点内。
   KEYWORD 为 `enum` 符号；NAME 为枚举名；UNDERLYING 为底层整数类型；
   BODY 为成员列表节点（CLUX_NODE_ENUM_VARIANTS）。 */
#define CLUX_EXPR_ENUM_DECL_KEYWORD 0
#define CLUX_EXPR_ENUM_DECL_NAME 1
#define CLUX_EXPR_ENUM_DECL_UNDERLYING 2
#define CLUX_EXPR_ENUM_DECL_BODY 3
#define CLUX_EXPR_ENUM_DECL_CHILDREN_SIZE 4

node_t *create_enum_decl_node(allocator_t *allocator);
void enum_decl_node_set_keyword(node_t *self, node_t *keyword);
node_t *enum_decl_node_get_keyword(node_t *self);
void enum_decl_node_set_name(node_t *self, node_t *name);
node_t *enum_decl_node_get_name(node_t *self);
/* UNDERLYING 允许为 NULL（容错：缺底层类型）。 */
void enum_decl_node_set_underlying(node_t *self, node_t *underlying);
node_t *enum_decl_node_get_underlying(node_t *self);
/* BODY 为 CLUX_NODE_ENUM_VARIANTS；容错时可为 NULL。 */
void enum_decl_node_set_body(node_t *self, node_t *body);
node_t *enum_decl_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
