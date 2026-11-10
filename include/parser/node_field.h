#ifndef _H_PARSER_NODE_FIELD_
#define _H_PARSER_NODE_FIELD_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 类型定义的字段 name: type。struct / tag union / cunion 共用。
   NAME 必有；TYPE 缺失时为 NULL（容错）。
   字段列表 { field; ... } 是 CLUX_NODE_FIELDS，children 为 CLUX_NODE_FIELD。 */
#define CLUX_FIELD_NAME 0
#define CLUX_FIELD_TYPE 1
#define CLUX_FIELD_CHILDREN_SIZE 2

node_t *create_field_node(allocator_t *allocator);
void field_node_set_name(node_t *self, node_t *name);
node_t *field_node_get_name(node_t *self);
/* TYPE 允许为 NULL（容错：缺字段类型）。 */
void field_node_set_type(node_t *self, node_t *type);
node_t *field_node_get_type(node_t *self);

/* 字段列表 { field; ... }，children 为 CLUX_NODE_FIELD。 */
node_t *create_fields_node(allocator_t *allocator);
void fields_node_add_field(node_t *self, node_t *field);
size_t fields_node_get_field_count(node_t *self);
/* 越界返回 NULL。 */
node_t *fields_node_get_field(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
