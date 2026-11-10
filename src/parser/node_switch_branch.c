#include "parser/node_switch_branch.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_switch_branch_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_SWITCH_BRANCH);
  node_resize_children(self, CLUX_SWITCH_BRANCH_CHILDREN_SIZE);
  return self;
}

void switch_branch_node_set_patterns(node_t *self, node_t *patterns) {
  node_set_child(self, CLUX_SWITCH_BRANCH_PATTERNS, patterns);
}

node_t *switch_branch_node_get_patterns(node_t *self) {
  return node_get_child(self, CLUX_SWITCH_BRANCH_PATTERNS);
}

void switch_branch_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_SWITCH_BRANCH_BODY, body);
}

node_t *switch_branch_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_SWITCH_BRANCH_BODY);
}

node_t *create_switch_branches_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_SWITCH_BRANCHES);
}

void switch_branches_node_add_branch(node_t *self, node_t *branch) {
  node_add_child(self, branch);
}

size_t switch_branches_node_get_branch_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *switch_branches_node_get_branch(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}

node_t *create_switch_patterns_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_SWITCH_PATTERNS);
}

void switch_patterns_node_add_pattern(node_t *self, node_t *pattern) {
  node_add_child(self, pattern);
}

size_t switch_patterns_node_get_pattern_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *switch_patterns_node_get_pattern(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}
