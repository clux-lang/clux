#include "parser/node_group.h"
#include "parser/node.h"

node_t *create_group_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_GROUP);
  node_resize_children(self, 1);
  return self;
}

void group_node_set_expr(node_t *self, node_t *expr) {
  node_set_child(self, CLUX_EXPR_GROUP_EXPR, expr);
}

node_t *group_node_get_expr(node_t *self) {
  return node_get_child(self, CLUX_EXPR_GROUP_EXPR);
}
