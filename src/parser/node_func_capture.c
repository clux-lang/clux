#include "parser/node_func_capture.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_func_capture_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_FUNC_CAPTURE);
  node_resize_children(self, CLUX_FUNC_CAPTURE_CHILDREN_SIZE);
  return self;
}

void func_capture_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_FUNC_CAPTURE_NAME, name);
}

node_t *func_capture_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_FUNC_CAPTURE_NAME);
}

void func_capture_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_FUNC_CAPTURE_TYPE, type);
}

node_t *func_capture_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_FUNC_CAPTURE_TYPE);
}

node_t *create_func_captures_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_FUNC_CAPTURES);
}

void func_captures_node_add_capture(node_t *self, node_t *capture) {
  node_add_child(self, capture);
}

size_t func_captures_node_get_capture_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *func_captures_node_get_capture(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}
