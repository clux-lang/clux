#include "parser/node_slice.h"
#include "parser/node.h"

node_t *create_slice_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_SLICE);
  node_resize_children(self, CLUX_EXPR_SLICE_CHILDREN_SIZE);
  return self;
}

void slice_node_set_base(node_t *self, node_t *base) {
  node_set_child(self, CLUX_EXPR_SLICE_BASE, base);
}

node_t *slice_node_get_base(node_t *self) {
  return node_get_child(self, CLUX_EXPR_SLICE_BASE);
}

void slice_node_set_lo(node_t *self, node_t *lo) {
  node_set_child(self, CLUX_EXPR_SLICE_LO, lo);
}

node_t *slice_node_get_lo(node_t *self) {
  return node_get_child(self, CLUX_EXPR_SLICE_LO);
}

void slice_node_set_hi(node_t *self, node_t *hi) {
  node_set_child(self, CLUX_EXPR_SLICE_HI, hi);
}

node_t *slice_node_get_hi(node_t *self) {
  return node_get_child(self, CLUX_EXPR_SLICE_HI);
}
