#include "parser/node_builtin.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_builtin_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_BUILTIN);
  node_resize_children(self, CLUX_EXPR_BUILTIN_FIRST_ARG);
  return self;
}

void builtin_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_BUILTIN_NAME, name);
}

node_t *builtin_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_BUILTIN_NAME);
}

void builtin_node_add_arg(node_t *self, node_t *arg) {
  node_add_child(self, arg);
}

size_t builtin_node_get_arg_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_EXPR_BUILTIN_FIRST_ARG ? size - CLUX_EXPR_BUILTIN_FIRST_ARG
                                            : 0;
}

node_t *builtin_node_get_arg(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_EXPR_BUILTIN_FIRST_ARG + idx);
}

void builtin_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprBuiltin");
  node_json_field(
      writer, "name", builtin_node_get_name(self), symbol_node_to_json);
  json_writer_key(writer, "args");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < builtin_node_get_arg_count(self); i++) {
    expr_to_json(writer, builtin_node_get_arg(self, i));
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
