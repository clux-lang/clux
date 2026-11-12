#include "parser/node_func_capture.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_func_capture_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_FUNC_CAPTURE);
  node_resize_children(self, CLUX_FUNC_CAPTURE_CHILDREN_SIZE);
  return self;
}

void func_capture_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_FUNC_CAPTURE_NAME, name);
}

node_t *func_capture_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_FUNC_CAPTURE_NAME);
}

void func_capture_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_FUNC_CAPTURE_TYPE, type);
}

node_t *func_capture_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_FUNC_CAPTURE_TYPE);
}

node_t *create_func_captures_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_FUNC_CAPTURES);
}

void func_captures_node_add_capture(node_t *self, node_t *capture) {
  node_add_child(self, capture);
}

size_t func_captures_node_get_capture_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *func_captures_node_get_capture(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}

void func_capture_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "FuncCapture");
  node_json_field(
      writer, "name", func_capture_node_get_name(self), expr_to_json);
  node_json_field(
      writer, "type", func_capture_node_get_type(self), expr_to_json);
  node_json_end(writer);
}

void func_captures_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "FuncCaptures");
  json_writer_key(writer, "captures");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < func_captures_node_get_capture_count(self); i++) {
    node_t *capture = func_captures_node_get_capture(self, i);
    if (node_get_kind(capture) == CLUX_NODE_FUNC_PARAMS) {
      func_params_node_to_json(writer, capture);
    } else {
      func_capture_node_to_json(writer, capture);
    }
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
