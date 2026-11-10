#ifndef _H_PARSER_NODE_ENUM_VARIANT_
#define _H_PARSER_NODE_ENUM_VARIANT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 枚举成员 Variant = value（白皮书 §3.6）。
   NAME 必有；VALUE 缺失时为 NULL（容错）。 */
#define CLUX_ENUM_VARIANT_NAME 0
#define CLUX_ENUM_VARIANT_VALUE 1
#define CLUX_ENUM_VARIANT_CHILDREN_SIZE 2

node_t *create_enum_variant_node(allocator_t *allocator);
void enum_variant_node_set_name(node_t *self, node_t *name);
node_t *enum_variant_node_get_name(node_t *self);
/* VALUE 允许为 NULL（容错：缺成员值）。 */
void enum_variant_node_set_value(node_t *self, node_t *value);
node_t *enum_variant_node_get_value(node_t *self);

/* 枚举成员列表 { Variant = value, ... }，children 为 CLUX_NODE_ENUM_VARIANT。 */
node_t *create_enum_variants_node(allocator_t *allocator);
void enum_variants_node_add_variant(node_t *self, node_t *variant);
size_t enum_variants_node_get_variant_count(node_t *self);
/* 越界返回 NULL。 */
node_t *enum_variants_node_get_variant(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
