#include "parser/node_stmt_empty.h"
#include "parser/node.h"

node_t *create_stmt_empty_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_STMT_EMPTY);
}

void stmt_empty_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtEmpty");
  node_json_end(writer);
}
