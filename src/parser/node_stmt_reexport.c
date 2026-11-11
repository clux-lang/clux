#include "parser/node_stmt_reexport.h"
#include "parser/node.h"

node_t *create_stmt_reexport_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_REEXPORT);
  node_resize_children(self, CLUX_STMT_REEXPORT_CHILDREN_SIZE);
  return self;
}

void stmt_reexport_node_set_path(node_t *self, node_t *path) {
  node_set_child(self, CLUX_STMT_REEXPORT_PATH, path);
}

node_t *stmt_reexport_node_get_path(node_t *self) {
  return node_get_child(self, CLUX_STMT_REEXPORT_PATH);
}
