#include "parser/node_call.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_call_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_CALL);
  node_resize_children(self, CLUX_EXPR_CALL_FIRST_ARG);
  return self;
}

void call_node_set_callee(node_t *self, node_t *callee) {
  node_set_child(self, CLUX_EXPR_CALL_CALLEE, callee);
}

node_t *call_node_get_callee(node_t *self) {
  return node_get_child(self, CLUX_EXPR_CALL_CALLEE);
}

void call_node_add_arg(node_t *self, node_t *arg) { node_add_child(self, arg); }

size_t call_node_get_arg_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_EXPR_CALL_FIRST_ARG ? size - CLUX_EXPR_CALL_FIRST_ARG : 0;
}

node_t *call_node_get_arg(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_EXPR_CALL_FIRST_ARG + idx);
}

void call_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprCall");
  node_json_field(writer, "callee", call_node_get_callee(self), expr_to_json);
  json_writer_key(writer, "args");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < call_node_get_arg_count(self); i++) {
    expr_to_json(writer, call_node_get_arg(self, i));
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
