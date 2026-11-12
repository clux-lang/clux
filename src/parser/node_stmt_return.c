#include "parser/node_stmt_return.h"
#include "parser/node.h"

node_t *create_stmt_return_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_RETURN);
  node_resize_children(self, CLUX_STMT_RETURN_CHILDREN_SIZE);
  return self;
}

void stmt_return_node_set_value(node_t *self, node_t *value) {
  node_set_child(self, CLUX_STMT_RETURN_VALUE, value);
}

node_t *stmt_return_node_get_value(node_t *self) {
  return node_get_child(self, CLUX_STMT_RETURN_VALUE);
}

void stmt_return_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtReturn");
  node_json_field(
      writer, "value", stmt_return_node_get_value(self), expr_to_json);
  node_json_end(writer);
}
