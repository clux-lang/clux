#ifndef _H_PARSER_NODE_BITFIELD_FIELD_
#define _H_PARSER_NODE_BITFIELD_FIELD_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 位域字段 name: width（白皮书 §10）。
   NAME 必有；WIDTH 缺失时为 NULL（容错）。 */
#define CLUX_BITFIELD_FIELD_NAME 0
#define CLUX_BITFIELD_FIELD_WIDTH 1
#define CLUX_BITFIELD_FIELD_CHILDREN_SIZE 2

node_t *create_bitfield_field_node(allocator_t *allocator);
void bitfield_field_node_set_name(node_t *self, node_t *name);
node_t *bitfield_field_node_get_name(node_t *self);
/* WIDTH 允许为 NULL（容错：缺字段宽度）。 */
void bitfield_field_node_set_width(node_t *self, node_t *width);
node_t *bitfield_field_node_get_width(node_t *self);

/* 位域字段列表 { name: width; ... }，children 为 CLUX_NODE_BITFIELD_FIELD。 */
node_t *create_bitfield_fields_node(allocator_t *allocator);
void bitfield_fields_node_add_field(node_t *self, node_t *field);
size_t bitfield_fields_node_get_field_count(node_t *self);
/* 越界返回 NULL。 */
node_t *bitfield_fields_node_get_field(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
