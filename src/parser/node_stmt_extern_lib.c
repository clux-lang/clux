#include "parser/node_stmt_extern_lib.h"
#include "parser/node.h"

node_t *create_stmt_extern_lib_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_EXTERN_LIB);
  node_resize_children(self, CLUX_STMT_EXTERN_LIB_CHILDREN_SIZE);
  return self;
}

void stmt_extern_lib_node_set_alias(node_t *self, node_t *alias) {
  node_set_child(self, CLUX_STMT_EXTERN_LIB_ALIAS, alias);
}

node_t *stmt_extern_lib_node_get_alias(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_LIB_ALIAS);
}

void stmt_extern_lib_node_set_path(node_t *self, node_t *path) {
  node_set_child(self, CLUX_STMT_EXTERN_LIB_PATH, path);
}

node_t *stmt_extern_lib_node_get_path(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_LIB_PATH);
}
