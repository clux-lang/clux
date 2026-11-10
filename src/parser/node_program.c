#include "parser/node_program.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_program_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_PROGRAM);
}

void program_node_add_stmt(node_t *self, node_t *stmt) {
  node_add_child(self, stmt);
}

size_t program_node_get_stmt_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *program_node_get_stmt(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}
