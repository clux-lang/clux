#include "parser/node_stmt_expr.h"
#include "parser/node.h"

node_t *create_stmt_expr_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_EXPR);
  node_resize_children(self, CLUX_STMT_EXPR_CHILDREN_SIZE);
  return self;
}

void stmt_expr_node_set_expr(node_t *self, node_t *expr) {
  node_set_child(self, CLUX_STMT_EXPR_EXPR, expr);
}

node_t *stmt_expr_node_get_expr(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXPR_EXPR);
}

void stmt_expr_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtExpr");
  node_json_field(writer, "expr", stmt_expr_node_get_expr(self), expr_to_json);
  node_json_end(writer);
}
