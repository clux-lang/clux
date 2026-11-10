#include "parser/node_construct.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_construct_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_CONSTRUCT);
  node_resize_children(self, CLUX_EXPR_CONSTRUCT_FIRST_ELEM);
  return self;
}

void construct_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_EXPR_CONSTRUCT_TYPE, type);
}

node_t *construct_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_EXPR_CONSTRUCT_TYPE);
}

void construct_node_add_elem(node_t *self, node_t *elem) {
  node_add_child(self, elem);
}

size_t construct_node_get_elem_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_EXPR_CONSTRUCT_FIRST_ELEM
             ? size - CLUX_EXPR_CONSTRUCT_FIRST_ELEM
             : 0;
}

node_t *construct_node_get_elem(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_EXPR_CONSTRUCT_FIRST_ELEM + idx);
}
