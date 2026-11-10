#include "parser/node_stmt_jump.h"
#include "parser/node.h"

node_t *create_stmt_break_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_STMT_BREAK);
}

node_t *create_stmt_continue_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_STMT_CONTINUE);
}
