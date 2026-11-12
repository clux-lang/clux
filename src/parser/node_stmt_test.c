#include "parser/node_stmt_test.h"
#include "parser/node.h"

node_t *create_stmt_test_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_TEST);
  node_resize_children(self, CLUX_STMT_TEST_CHILDREN_SIZE);
  return self;
}

void stmt_test_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_STMT_TEST_NAME, name);
}

node_t *stmt_test_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_STMT_TEST_NAME);
}

void stmt_test_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_STMT_TEST_BODY, body);
}

node_t *stmt_test_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_STMT_TEST_BODY);
}

void stmt_test_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtTest");
  node_json_field(writer, "name", stmt_test_node_get_name(self), expr_to_json);
  node_json_field(
      writer, "body", stmt_test_node_get_body(self), stmt_block_node_to_json);
  node_json_end(writer);
}
