#include "parser/node_stmt_jump.h"
#include "parser/node.h"

node_t *create_stmt_break_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_STMT_BREAK);
}

node_t *create_stmt_continue_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_STMT_CONTINUE);
}

void stmt_break_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtBreak");
  node_json_end(writer);
}

void stmt_continue_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtContinue");
  node_json_end(writer);
}
