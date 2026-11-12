#include "parser/node_binary.h"
#include "parser/node.h"

static node_t *create_binary_shape(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(self, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  return self;
}

node_t *create_binary_node(allocator_t *allocator) {
  return create_binary_shape(allocator);
}

node_t *create_unary_node(allocator_t *allocator) {
  /* 前缀形态：LEFT 保持 NULL。 */
  return create_binary_shape(allocator);
}

node_t *create_postfix_node(allocator_t *allocator) {
  /* 后缀形态：RIGHT 保持 NULL。 */
  return create_binary_shape(allocator);
}

void binary_node_set_left(node_t *self, node_t *left) {
  node_set_child(self, CLUX_EXPR_BINARY_LEFT, left);
}

node_t *binary_node_get_left(node_t *self) {
  return node_get_child(self, CLUX_EXPR_BINARY_LEFT);
}

void binary_node_set_symbol(node_t *self, node_t *symbol) {
  node_set_child(self, CLUX_EXPR_BINARY_SYMBOL, symbol);
}

node_t *binary_node_get_symbol(node_t *self) {
  return node_get_child(self, CLUX_EXPR_BINARY_SYMBOL);
}

void binary_node_set_right(node_t *self, node_t *right) {
  node_set_child(self, CLUX_EXPR_BINARY_RIGHT, right);
}

node_t *binary_node_get_right(node_t *self) {
  return node_get_child(self, CLUX_EXPR_BINARY_RIGHT);
}

void binary_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprBinary");
  node_json_field(writer, "left", binary_node_get_left(self), expr_to_json);
  node_json_field(
      writer, "symbol", binary_node_get_symbol(self), symbol_node_to_json);
  node_json_field(writer, "right", binary_node_get_right(self), expr_to_json);
  node_json_end(writer);
}
