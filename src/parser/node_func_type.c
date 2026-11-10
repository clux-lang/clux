#include "parser/node_func_type.h"
#include "parser/node.h"

node_t *create_func_type_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_FUNC_TYPE);
  node_resize_children(self, CLUX_EXPR_FUNC_TYPE_CHILDREN_SIZE);
  return self;
}

void func_type_node_set_captures(node_t *self, node_t *captures) {
  node_set_child(self, CLUX_EXPR_FUNC_TYPE_CAPTURES, captures);
}

node_t *func_type_node_get_captures(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_TYPE_CAPTURES);
}

void func_type_node_set_params(node_t *self, node_t *params) {
  node_set_child(self, CLUX_EXPR_FUNC_TYPE_PARAMS, params);
}

node_t *func_type_node_get_params(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_TYPE_PARAMS);
}

void func_type_node_set_return(node_t *self, node_t *ret) {
  node_set_child(self, CLUX_EXPR_FUNC_TYPE_RETURN, ret);
}

node_t *func_type_node_get_return(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_TYPE_RETURN);
}
