#ifndef _H_PARSER_NODE_LITERAL_
#define _H_PARSER_NODE_LITERAL_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 字面量节点全部是叶子：无子节点，值由 location 指向的源码文本决定。
   每种字面量一个工厂，避免调用方用裸 kind 直接 create_node。 */
node_t *create_literal_nil_node(allocator_t *allocator);
node_t *create_literal_undefined_node(allocator_t *allocator);
node_t *create_literal_string_node(allocator_t *allocator);
node_t *create_literal_character_node(allocator_t *allocator);
node_t *create_literal_numeric_node(allocator_t *allocator);
node_t *create_literal_boolean_node(allocator_t *allocator);
node_t *create_literal_identifier_node(allocator_t *allocator);
/* 内建类型名（i8..f64、isize/usize、bool/str/void/opaque）。 */
node_t *create_literal_type_node(allocator_t *allocator);
#ifdef __cplusplus
}
#endif
#endif
