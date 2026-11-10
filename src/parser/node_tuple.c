#include "parser/node_tuple.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_tuple_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_EXPR_TUPLE);
}

void tuple_node_add_elem(node_t *self, node_t *elem) {
  node_add_child(self, elem);
}

size_t tuple_node_get_elem_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *tuple_node_get_elem(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}
