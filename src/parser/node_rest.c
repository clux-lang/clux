#include "parser/node_rest.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_rest_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_EXPR_REST);
}

void rest_node_set_expr(node_t *self, node_t *expr) {
  if (vec_get_size(node_get_children(self)) == 0) {
    node_resize_children(self, 1);
  }
  node_set_child(self, CLUX_EXPR_REST_EXPR, expr);
}

node_t *rest_node_get_expr(node_t *self) {
  return node_get_child(self, CLUX_EXPR_REST_EXPR);
}

void rest_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprRest");
  node_json_field(writer, "expr", rest_node_get_expr(self), expr_to_json);
  node_json_end(writer);
}
