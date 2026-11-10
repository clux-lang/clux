#include "parser/node_generic_param.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_generic_param_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_GENERIC_PARAM);
  node_resize_children(self, CLUX_GENERIC_PARAM_CHILDREN_SIZE);
  return self;
}

void generic_param_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_GENERIC_PARAM_NAME, name);
}

node_t *generic_param_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_GENERIC_PARAM_NAME);
}

void generic_param_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_GENERIC_PARAM_TYPE, type);
}

node_t *generic_param_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_GENERIC_PARAM_TYPE);
}

void generic_param_node_set_constraint(node_t *self, node_t *constraint) {
  node_set_child(self, CLUX_GENERIC_PARAM_CONSTRAINT, constraint);
}

node_t *generic_param_node_get_constraint(node_t *self) {
  return node_get_child(self, CLUX_GENERIC_PARAM_CONSTRAINT);
}

void generic_param_node_set_default(node_t *self, node_t *default_value) {
  node_set_child(self, CLUX_GENERIC_PARAM_DEFAULT, default_value);
}

node_t *generic_param_node_get_default(node_t *self) {
  return node_get_child(self, CLUX_GENERIC_PARAM_DEFAULT);
}

node_t *create_generic_params_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_GENERIC_PARAMS);
}

void generic_params_node_add_param(node_t *self, node_t *param) {
  node_add_child(self, param);
}

size_t generic_params_node_get_param_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *generic_params_node_get_param(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}
