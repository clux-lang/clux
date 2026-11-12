#include "parser/node_construct_field.h"
#include "parser/node.h"

node_t *create_construct_field_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_CONSTRUCT_FIELD);
  node_resize_children(self, 2);
  return self;
}

void construct_field_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_CONSTRUCT_FIELD_NAME, name);
}

node_t *construct_field_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_CONSTRUCT_FIELD_NAME);
}

void construct_field_node_set_value(node_t *self, node_t *value) {
  node_set_child(self, CLUX_EXPR_CONSTRUCT_FIELD_VALUE, value);
}

node_t *construct_field_node_get_value(node_t *self) {
  return node_get_child(self, CLUX_EXPR_CONSTRUCT_FIELD_VALUE);
}

void construct_field_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprConstructField");
  node_json_field(
      writer, "name", construct_field_node_get_name(self), expr_to_json);
  node_json_field(
      writer, "value", construct_field_node_get_value(self), expr_to_json);
  node_json_end(writer);
}
