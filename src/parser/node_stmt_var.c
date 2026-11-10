#include "parser/node_stmt_var.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_stmt_var_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_VAR);
  node_resize_children(self, CLUX_STMT_VAR_FIRST_DECORATOR);
  return self;
}

void stmt_var_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_STMT_VAR_KEYWORD, keyword);
}

node_t *stmt_var_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_STMT_VAR_KEYWORD);
}

void stmt_var_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_STMT_VAR_NAME, name);
}

node_t *stmt_var_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_STMT_VAR_NAME);
}

void stmt_var_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_STMT_VAR_TYPE, type);
}

node_t *stmt_var_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_STMT_VAR_TYPE);
}

void stmt_var_node_set_value(node_t *self, node_t *value) {
  node_set_child(self, CLUX_STMT_VAR_VALUE, value);
}

node_t *stmt_var_node_get_value(node_t *self) {
  return node_get_child(self, CLUX_STMT_VAR_VALUE);
}

void stmt_var_node_set_comptime(node_t *self, node_t *comptime) {
  node_set_child(self, CLUX_STMT_VAR_COMPTIME, comptime);
}

node_t *stmt_var_node_get_comptime(node_t *self) {
  return node_get_child(self, CLUX_STMT_VAR_COMPTIME);
}

void stmt_var_node_add_decorator(node_t *self, node_t *decorator) {
  node_add_child(self, decorator);
}

size_t stmt_var_node_get_decorator_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_STMT_VAR_FIRST_DECORATOR
             ? size - CLUX_STMT_VAR_FIRST_DECORATOR
             : 0;
}

node_t *stmt_var_node_get_decorator(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_STMT_VAR_FIRST_DECORATOR + idx);
}
