#ifndef _H_PARSER_NODE_BITFIELD_DECL_
#define _H_PARSER_NODE_BITFIELD_DECL_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 位域声明 bitfield Name: Underlying { field: width; ... }（白皮书 §10）。
   装饰器属于外层语句（stmt_bitfield），不在本节点内。
   KEYWORD 为 `bitfield` 符号；NAME 为位域名；UNDERLYING 为底层无符号类型；
   BODY 为字段列表节点（CLUX_NODE_BITFIELD_FIELDS）。 */
#define CLUX_EXPR_BITFIELD_DECL_KEYWORD 0
#define CLUX_EXPR_BITFIELD_DECL_NAME 1
#define CLUX_EXPR_BITFIELD_DECL_UNDERLYING 2
#define CLUX_EXPR_BITFIELD_DECL_BODY 3
#define CLUX_EXPR_BITFIELD_DECL_CHILDREN_SIZE 4

node_t *create_bitfield_decl_node(allocator_t *allocator);
void bitfield_decl_node_set_keyword(node_t *self, node_t *keyword);
node_t *bitfield_decl_node_get_keyword(node_t *self);
void bitfield_decl_node_set_name(node_t *self, node_t *name);
node_t *bitfield_decl_node_get_name(node_t *self);
/* UNDERLYING 允许为 NULL（容错：缺底层类型）。 */
void bitfield_decl_node_set_underlying(node_t *self, node_t *underlying);
node_t *bitfield_decl_node_get_underlying(node_t *self);
/* BODY 为 CLUX_NODE_BITFIELD_FIELDS；容错时可为 NULL。 */
void bitfield_decl_node_set_body(node_t *self, node_t *body);
node_t *bitfield_decl_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
