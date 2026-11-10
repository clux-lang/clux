#include "parser/node_assign.h"
#include "parser/node.h"

node_t *create_assign_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_ASSIGN);
  node_resize_children(self, CLUX_EXPR_ASSIGN_CHILDREN_SIZE);
  return self;
}

void assign_node_set_id(node_t *self, node_t *id) {
  node_set_child(self, CLUX_EXPR_ASSIGN_ID, id);
}

node_t *assign_node_get_id(node_t *self) {
  return node_get_child(self, CLUX_EXPR_ASSIGN_ID);
}

void assign_node_set_symbol(node_t *self, node_t *symbol) {
  node_set_child(self, CLUX_EXPR_ASSIGN_SYMBOL, symbol);
}

node_t *assign_node_get_symbol(node_t *self) {
  return node_get_child(self, CLUX_EXPR_ASSIGN_SYMBOL);
}

void assign_node_set_value(node_t *self, node_t *value) {
  node_set_child(self, CLUX_EXPR_ASSIGN_VALUE, value);
}

node_t *assign_node_get_value(node_t *self) {
  return node_get_child(self, CLUX_EXPR_ASSIGN_VALUE);
}
