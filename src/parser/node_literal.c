#include "parser/node_literal.h"
#include "parser/node.h"

node_t *create_literal_nil_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_LITERAL_NIL);
}

node_t *create_literal_undefined_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_LITERAL_UNDEFINED);
}

node_t *create_literal_string_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_LITERAL_STRING);
}

node_t *create_literal_character_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_LITERAL_CHARACTER);
}

node_t *create_literal_numeric_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_LITERAL_NUMERIC);
}

node_t *create_literal_boolean_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_LITERAL_BOOLEAN);
}

node_t *create_literal_identifier_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER);
}

node_t *create_literal_type_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_LITERAL_TYPE);
}
