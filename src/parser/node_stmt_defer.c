#include "parser/node_stmt_defer.h"
#include "parser/node.h"

node_t *create_stmt_defer_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_DEFER);
  node_resize_children(self, CLUX_STMT_DEFER_CHILDREN_SIZE);
  return self;
}

void stmt_defer_node_set_captures(node_t *self, node_t *captures) {
  node_set_child(self, CLUX_STMT_DEFER_CAPTURES, captures);
}

node_t *stmt_defer_node_get_captures(node_t *self) {
  return node_get_child(self, CLUX_STMT_DEFER_CAPTURES);
}

void stmt_defer_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_STMT_DEFER_BODY, body);
}

node_t *stmt_defer_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_STMT_DEFER_BODY);
}

void stmt_defer_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtDefer");
  node_json_field(writer,
                  "captures",
                  stmt_defer_node_get_captures(self),
                  func_captures_node_to_json);
  node_json_field(
      writer, "body", stmt_defer_node_get_body(self), stmt_block_node_to_json);
  node_json_end(writer);
}
