#include "parser/node_stmt_switch.h"
#include "parser/node.h"

node_t *create_stmt_switch_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_SWITCH);
  node_resize_children(self, CLUX_STMT_SWITCH_CHILDREN_SIZE);
  return self;
}

void stmt_switch_node_set_value(node_t *self, node_t *value) {
  node_set_child(self, CLUX_STMT_SWITCH_VALUE, value);
}

node_t *stmt_switch_node_get_value(node_t *self) {
  return node_get_child(self, CLUX_STMT_SWITCH_VALUE);
}

void stmt_switch_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_STMT_SWITCH_BODY, body);
}

node_t *stmt_switch_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_STMT_SWITCH_BODY);
}
