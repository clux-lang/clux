#include "parser/node_stmt_func.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_stmt_func_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_FUNC);
  node_resize_children(self, CLUX_STMT_FUNC_FIRST_DECORATOR);
  return self;
}

void stmt_func_node_set_func_decl(node_t *self, node_t *func_decl) {
  node_set_child(self, CLUX_STMT_FUNC_FUNC_DECL, func_decl);
}

node_t *stmt_func_node_get_func_decl(node_t *self) {
  return node_get_child(self, CLUX_STMT_FUNC_FUNC_DECL);
}

void stmt_func_node_set_comptime(node_t *self, node_t *comptime) {
  node_set_child(self, CLUX_STMT_FUNC_COMPTIME, comptime);
}

node_t *stmt_func_node_get_comptime(node_t *self) {
  return node_get_child(self, CLUX_STMT_FUNC_COMPTIME);
}

void stmt_func_node_add_decorator(node_t *self, node_t *decorator) {
  node_add_child(self, decorator);
}

size_t stmt_func_node_get_decorator_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_STMT_FUNC_FIRST_DECORATOR
             ? size - CLUX_STMT_FUNC_FIRST_DECORATOR
             : 0;
}

node_t *stmt_func_node_get_decorator(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_STMT_FUNC_FIRST_DECORATOR + idx);
}
