#include "parser/node_struct_decl.h"
#include "parser/node.h"

node_t *create_struct_decl_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_STRUCT_DECL);
  node_resize_children(self, CLUX_EXPR_STRUCT_DECL_CHILDREN_SIZE);
  return self;
}

void struct_decl_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_KEYWORD, keyword);
}

node_t *struct_decl_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_KEYWORD);
}

void struct_decl_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_NAME, name);
}

node_t *struct_decl_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_NAME);
}

void struct_decl_node_set_generics(node_t *self, node_t *generics) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_GENERICS, generics);
}

node_t *struct_decl_node_get_generics(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_GENERICS);
}

void struct_decl_node_set_implements(node_t *self, node_t *implements) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_IMPLEMENTS, implements);
}

node_t *struct_decl_node_get_implements(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_IMPLEMENTS);
}

void struct_decl_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_BODY, body);
}

node_t *struct_decl_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_BODY);
}
