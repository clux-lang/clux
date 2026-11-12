#include "parser/node_implements.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_implements_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_IMPLEMENTS);
}

void implements_node_add_type(node_t *self, node_t *type) {
  node_add_child(self, type);
}

size_t implements_node_get_type_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *implements_node_get_type(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}

void implements_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "Implements");
  json_writer_key(writer, "interfaces");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < implements_node_get_type_count(self); i++) {
    expr_to_json(writer, implements_node_get_type(self, i));
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
