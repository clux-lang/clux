#include "parser/node_assign.h"
#include "parser/node.h"

node_t *create_assign_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_ASSIGN);
  node_resize_children(self, CLUX_EXPR_ASSIGN_CHILDREN_SIZE);
  return self;
}

void assign_node_set_left(node_t *self, node_t *left) {
  node_set_child(self, CLUX_EXPR_ASSIGN_LEFT, left);
}

node_t *assign_node_get_left(node_t *self) {
  return node_get_child(self, CLUX_EXPR_ASSIGN_LEFT);
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

void assign_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprAssign");
  node_json_field(writer, "left", assign_node_get_left(self), expr_to_json);
  node_json_field(
      writer, "symbol", assign_node_get_symbol(self), symbol_node_to_json);
  node_json_field(writer, "value", assign_node_get_value(self), expr_to_json);
  node_json_end(writer);
}
