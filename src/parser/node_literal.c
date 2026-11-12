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

static void
write_literal(json_writer_t *writer, node_t *self, const char *kind) {
  node_json_begin(writer, self, kind);
  node_json_text(writer, self);
  node_json_end(writer);
}

void literal_nil_node_to_json(json_writer_t *writer, node_t *self) {
  write_literal(writer, self, "LiteralNil");
}
void literal_undefined_node_to_json(json_writer_t *writer, node_t *self) {
  write_literal(writer, self, "LiteralUndefined");
}
void literal_string_node_to_json(json_writer_t *writer, node_t *self) {
  write_literal(writer, self, "LiteralString");
}
void literal_character_node_to_json(json_writer_t *writer, node_t *self) {
  write_literal(writer, self, "LiteralCharacter");
}
void literal_numeric_node_to_json(json_writer_t *writer, node_t *self) {
  write_literal(writer, self, "LiteralNumeric");
}
void literal_boolean_node_to_json(json_writer_t *writer, node_t *self) {
  write_literal(writer, self, "LiteralBoolean");
}
void literal_identifier_node_to_json(json_writer_t *writer, node_t *self) {
  write_literal(writer, self, "LiteralIdentifier");
}
void literal_type_node_to_json(json_writer_t *writer, node_t *self) {
  write_literal(writer, self, "LiteralType");
}
