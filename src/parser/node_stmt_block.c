#include "parser/node_stmt_block.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_stmt_block_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_STMT_BLOCK);
}

void stmt_block_node_add_stmt(node_t *self, node_t *stmt) {
  node_add_child(self, stmt);
}

size_t stmt_block_node_get_stmt_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *stmt_block_node_get_stmt(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}

void stmt_block_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtBlock");
  json_writer_key(writer, "stmts");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < stmt_block_node_get_stmt_count(self); i++) {
    stmt_to_json(writer, stmt_block_node_get_stmt(self, i));
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
