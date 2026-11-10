#include "parser/node_stmt_type.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_stmt_type_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_TYPE);
  node_resize_children(self, CLUX_STMT_TYPE_FIRST_DECORATOR);
  return self;
}

void stmt_type_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_STMT_TYPE_NAME, name);
}

node_t *stmt_type_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_STMT_TYPE_NAME);
}

void stmt_type_node_set_generics(node_t *self, node_t *generics) {
  node_set_child(self, CLUX_STMT_TYPE_GENERICS, generics);
}

node_t *stmt_type_node_get_generics(node_t *self) {
  return node_get_child(self, CLUX_STMT_TYPE_GENERICS);
}

void stmt_type_node_set_value(node_t *self, node_t *value) {
  node_set_child(self, CLUX_STMT_TYPE_VALUE, value);
}

node_t *stmt_type_node_get_value(node_t *self) {
  return node_get_child(self, CLUX_STMT_TYPE_VALUE);
}

void stmt_type_node_add_decorator(node_t *self, node_t *decorator) {
  node_add_child(self, decorator);
}

size_t stmt_type_node_get_decorator_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_STMT_TYPE_FIRST_DECORATOR
             ? size - CLUX_STMT_TYPE_FIRST_DECORATOR
             : 0;
}

node_t *stmt_type_node_get_decorator(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_STMT_TYPE_FIRST_DECORATOR + idx);
}
