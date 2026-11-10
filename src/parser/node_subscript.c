#include "parser/node_subscript.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_subscript_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_SUBSCRIPT);
  node_resize_children(self, CLUX_EXPR_SUBSCRIPT_FIRST_ARG);
  return self;
}

void subscript_node_set_base(node_t *self, node_t *base) {
  node_set_child(self, CLUX_EXPR_SUBSCRIPT_BASE, base);
}

node_t *subscript_node_get_base(node_t *self) {
  return node_get_child(self, CLUX_EXPR_SUBSCRIPT_BASE);
}

void subscript_node_add_arg(node_t *self, node_t *arg) {
  node_add_child(self, arg);
}

size_t subscript_node_get_arg_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_EXPR_SUBSCRIPT_FIRST_ARG
             ? size - CLUX_EXPR_SUBSCRIPT_FIRST_ARG
             : 0;
}

node_t *subscript_node_get_arg(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_EXPR_SUBSCRIPT_FIRST_ARG + idx);
}
