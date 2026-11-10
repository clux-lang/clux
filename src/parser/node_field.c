#include "parser/node_field.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_field_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_FIELD);
  node_resize_children(self, CLUX_FIELD_CHILDREN_SIZE);
  return self;
}

void field_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_FIELD_NAME, name);
}

node_t *field_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_FIELD_NAME);
}

void field_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_FIELD_TYPE, type);
}

node_t *field_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_FIELD_TYPE);
}

node_t *create_fields_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_FIELDS);
}

void fields_node_add_field(node_t *self, node_t *field) {
  node_add_child(self, field);
}

size_t fields_node_get_field_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *fields_node_get_field(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}
