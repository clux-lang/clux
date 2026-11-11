#include "parser/node_interface_decl.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_interface_decl_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_INTERFACE_DECL);
  node_resize_children(self, CLUX_EXPR_INTERFACE_DECL_CHILDREN_SIZE);
  return self;
}

void interface_decl_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_EXPR_INTERFACE_DECL_KEYWORD, keyword);
}

node_t *interface_decl_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_EXPR_INTERFACE_DECL_KEYWORD);
}

void interface_decl_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_INTERFACE_DECL_NAME, name);
}

node_t *interface_decl_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_INTERFACE_DECL_NAME);
}

void interface_decl_node_set_generics(node_t *self, node_t *generics) {
  node_set_child(self, CLUX_EXPR_INTERFACE_DECL_GENERICS, generics);
}

node_t *interface_decl_node_get_generics(node_t *self) {
  return node_get_child(self, CLUX_EXPR_INTERFACE_DECL_GENERICS);
}

void interface_decl_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_EXPR_INTERFACE_DECL_BODY, body);
}

node_t *interface_decl_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_EXPR_INTERFACE_DECL_BODY);
}

node_t *create_interface_members_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_INTERFACE_MEMBERS);
}

void interface_members_node_add_member(node_t *self, node_t *member) {
  node_add_child(self, member);
}

size_t interface_members_node_get_member_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *interface_members_node_get_member(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}
