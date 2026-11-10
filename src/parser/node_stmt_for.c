#include "parser/node_stmt_for.h"
#include "parser/node.h"

node_t *create_stmt_for_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_FOR);
  node_resize_children(self, CLUX_STMT_FOR_CHILDREN_SIZE);
  return self;
}

void stmt_for_node_set_init(node_t *self, node_t *init) {
  node_set_child(self, CLUX_STMT_FOR_INIT, init);
}

node_t *stmt_for_node_get_init(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOR_INIT);
}

void stmt_for_node_set_condition(node_t *self, node_t *condition) {
  node_set_child(self, CLUX_STMT_FOR_CONDITION, condition);
}

node_t *stmt_for_node_get_condition(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOR_CONDITION);
}

void stmt_for_node_set_update(node_t *self, node_t *update) {
  node_set_child(self, CLUX_STMT_FOR_UPDATE, update);
}

node_t *stmt_for_node_get_update(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOR_UPDATE);
}

void stmt_for_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_STMT_FOR_BODY, body);
}

node_t *stmt_for_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOR_BODY);
}
