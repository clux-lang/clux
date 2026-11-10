#include "parser/node_stmt_do_while.h"
#include "parser/node.h"

node_t *create_stmt_do_while_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_DO_WHILE);
  node_resize_children(self, CLUX_STMT_DO_WHILE_CHILDREN_SIZE);
  return self;
}

void stmt_do_while_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_STMT_DO_WHILE_BODY, body);
}

node_t *stmt_do_while_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_STMT_DO_WHILE_BODY);
}

void stmt_do_while_node_set_condition(node_t *self, node_t *condition) {
  node_set_child(self, CLUX_STMT_DO_WHILE_CONDITION, condition);
}

node_t *stmt_do_while_node_get_condition(node_t *self) {
  return node_get_child(self, CLUX_STMT_DO_WHILE_CONDITION);
}
