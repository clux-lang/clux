#include "parser/node_func_decl.h"
#include "parser/node.h"

node_t *create_func_decl_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_FUNC_DECL);
  node_resize_children(self, CLUX_EXPR_FUNC_DECL_CHILDREN_SIZE);
  return self;
}

void func_decl_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_EXPR_FUNC_DECL_KEYWORD, keyword);
}

node_t *func_decl_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_DECL_KEYWORD);
}

void func_decl_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_FUNC_DECL_NAME, name);
}

node_t *func_decl_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_DECL_NAME);
}

void func_decl_node_set_generics(node_t *self, node_t *generics) {
  node_set_child(self, CLUX_EXPR_FUNC_DECL_GENERICS, generics);
}

node_t *func_decl_node_get_generics(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_DECL_GENERICS);
}

void func_decl_node_set_captures(node_t *self, node_t *captures) {
  node_set_child(self, CLUX_EXPR_FUNC_DECL_CAPTURES, captures);
}

node_t *func_decl_node_get_captures(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_DECL_CAPTURES);
}

void func_decl_node_set_params(node_t *self, node_t *params) {
  node_set_child(self, CLUX_EXPR_FUNC_DECL_PARAMS, params);
}

node_t *func_decl_node_get_params(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_DECL_PARAMS);
}

void func_decl_node_set_return(node_t *self, node_t *ret) {
  node_set_child(self, CLUX_EXPR_FUNC_DECL_RETURN, ret);
}

node_t *func_decl_node_get_return(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_DECL_RETURN);
}

void func_decl_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_EXPR_FUNC_DECL_BODY, body);
}

node_t *func_decl_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_EXPR_FUNC_DECL_BODY);
}
