#include "parser/node_decorator.h"
#include "parser/node.h"

node_t *create_decorator_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_DECORATOR);
  node_resize_children(self, CLUX_DECORATOR_CHILDREN_SIZE);
  return self;
}

void decorator_node_set_expr(node_t *self, node_t *expr) {
  node_set_child(self, CLUX_DECORATOR_EXPR, expr);
}

node_t *decorator_node_get_expr(node_t *self) {
  return node_get_child(self, CLUX_DECORATOR_EXPR);
}

void decorator_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "Decorator");
  node_json_field(writer, "expr", decorator_node_get_expr(self), expr_to_json);
  node_json_end(writer);
}
