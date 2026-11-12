#include "parser/node_ternary.h"
#include "parser/node.h"

node_t *create_ternary_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_TERNARY);
  node_resize_children(self, CLUX_EXPR_TERNARY_CHILDREN_SIZE);
  return self;
}

void ternary_node_set_condition(node_t *self, node_t *condition) {
  node_set_child(self, CLUX_EXPR_TERNARY_CONDITION, condition);
}

node_t *ternary_node_get_condition(node_t *self) {
  return node_get_child(self, CLUX_EXPR_TERNARY_CONDITION);
}

void ternary_node_set_then(node_t *self, node_t *then) {
  node_set_child(self, CLUX_EXPR_TERNARY_THEN, then);
}

node_t *ternary_node_get_then(node_t *self) {
  return node_get_child(self, CLUX_EXPR_TERNARY_THEN);
}

void ternary_node_set_else(node_t *self, node_t *else_) {
  node_set_child(self, CLUX_EXPR_TERNARY_ELSE, else_);
}

node_t *ternary_node_get_else(node_t *self) {
  return node_get_child(self, CLUX_EXPR_TERNARY_ELSE);
}

void ternary_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprTernary");
  node_json_field(
      writer, "condition", ternary_node_get_condition(self), expr_to_json);
  node_json_field(writer, "then", ternary_node_get_then(self), expr_to_json);
  node_json_field(writer, "else", ternary_node_get_else(self), expr_to_json);
  node_json_end(writer);
}
