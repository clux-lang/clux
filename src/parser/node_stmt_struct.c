#include "parser/node_stmt_struct.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_stmt_struct_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_STRUCT);
  node_resize_children(self, CLUX_STMT_STRUCT_FIRST_DECORATOR);
  return self;
}

void stmt_struct_node_set_struct_decl(node_t *self, node_t *struct_decl) {
  node_set_child(self, CLUX_STMT_STRUCT_STRUCT_DECL, struct_decl);
}

node_t *stmt_struct_node_get_struct_decl(node_t *self) {
  return node_get_child(self, CLUX_STMT_STRUCT_STRUCT_DECL);
}

void stmt_struct_node_set_export(node_t *self, node_t *export_node) {
  node_set_child(self, CLUX_STMT_STRUCT_EXPORT, export_node);
}

node_t *stmt_struct_node_get_export(node_t *self) {
  return node_get_child(self, CLUX_STMT_STRUCT_EXPORT);
}

void stmt_struct_node_add_decorator(node_t *self, node_t *decorator) {
  node_add_child(self, decorator);
}

size_t stmt_struct_node_get_decorator_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_STMT_STRUCT_FIRST_DECORATOR
             ? size - CLUX_STMT_STRUCT_FIRST_DECORATOR
             : 0;
}

node_t *stmt_struct_node_get_decorator(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_STMT_STRUCT_FIRST_DECORATOR + idx);
}
