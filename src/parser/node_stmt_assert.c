#include "parser/node_stmt_assert.h"
#include "parser/node.h"

node_t *create_stmt_assert_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_ASSERT);
  node_resize_children(self, CLUX_STMT_ASSERT_CHILDREN_SIZE);
  return self;
}

void stmt_assert_node_set_condition(node_t *self, node_t *condition) {
  node_set_child(self, CLUX_STMT_ASSERT_CONDITION, condition);
}

node_t *stmt_assert_node_get_condition(node_t *self) {
  return node_get_child(self, CLUX_STMT_ASSERT_CONDITION);
}

void stmt_assert_node_set_source(node_t *self, node_t *source) {
  node_set_child(self, CLUX_STMT_ASSERT_SOURCE, source);
}

node_t *stmt_assert_node_get_source(node_t *self) {
  return node_get_child(self, CLUX_STMT_ASSERT_SOURCE);
}

void stmt_assert_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtAssert");
  node_json_field(
      writer, "condition", stmt_assert_node_get_condition(self), expr_to_json);
  node_json_field(
      writer, "source", stmt_assert_node_get_source(self), expr_to_json);
  node_json_end(writer);
}
