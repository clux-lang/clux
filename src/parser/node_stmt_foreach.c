#include "parser/node_stmt_foreach.h"
#include "parser/node.h"

node_t *create_stmt_foreach_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_FOREACH);
  node_resize_children(self, CLUX_STMT_FOREACH_CHILDREN_SIZE);
  return self;
}

node_t *create_stmt_comptime_foreach_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_COMPTIME_FOREACH);
  node_resize_children(self, CLUX_STMT_FOREACH_CHILDREN_SIZE);
  return self;
}

void stmt_foreach_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_STMT_FOREACH_KEYWORD, keyword);
}

node_t *stmt_foreach_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOREACH_KEYWORD);
}

void stmt_foreach_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_STMT_FOREACH_NAME, name);
}

node_t *stmt_foreach_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOREACH_NAME);
}

void stmt_foreach_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_STMT_FOREACH_TYPE, type);
}

node_t *stmt_foreach_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOREACH_TYPE);
}

void stmt_foreach_node_set_iterable(node_t *self, node_t *iterable) {
  node_set_child(self, CLUX_STMT_FOREACH_ITERABLE, iterable);
}

node_t *stmt_foreach_node_get_iterable(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOREACH_ITERABLE);
}

void stmt_foreach_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_STMT_FOREACH_BODY, body);
}

node_t *stmt_foreach_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_STMT_FOREACH_BODY);
}
