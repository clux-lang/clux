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

void tuple_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprTuple");
  json_writer_key(writer, "elements");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < tuple_node_get_elem_count(self); i++) {
    expr_to_json(writer, tuple_node_get_elem(self, i));
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
