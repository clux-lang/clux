#include "parser/node_stmt_if.h"
#include "parser/node.h"

node_t *create_stmt_if_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_IF);
  node_resize_children(self, CLUX_STMT_IF_CHILDREN_SIZE);
  return self;
}

node_t *create_stmt_comptime_if_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_COMPTIME_IF);
  node_resize_children(self, CLUX_STMT_IF_CHILDREN_SIZE);
  return self;
}

void stmt_if_node_set_condition(node_t *self, node_t *condition) {
  node_set_child(self, CLUX_STMT_IF_CONDITION, condition);
}

node_t *stmt_if_node_get_condition(node_t *self) {
  return node_get_child(self, CLUX_STMT_IF_CONDITION);
}

void stmt_if_node_set_then(node_t *self, node_t *then_branch) {
  node_set_child(self, CLUX_STMT_IF_THEN, then_branch);
}

node_t *stmt_if_node_get_then(node_t *self) {
  return node_get_child(self, CLUX_STMT_IF_THEN);
}

void stmt_if_node_set_else(node_t *self, node_t *else_branch) {
  node_set_child(self, CLUX_STMT_IF_ELSE, else_branch);
}

node_t *stmt_if_node_get_else(node_t *self) {
  return node_get_child(self, CLUX_STMT_IF_ELSE);
}
