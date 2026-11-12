#include "parser/node_field.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_field_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_FIELD);
  node_resize_children(self, CLUX_FIELD_FIRST_DECORATOR);
  return self;
}

void field_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_FIELD_NAME, name);
}

node_t *field_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_FIELD_NAME);
}

void field_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_FIELD_TYPE, type);
}

node_t *field_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_FIELD_TYPE);
}

void field_node_add_decorator(node_t *self, node_t *decorator) {
  node_add_child(self, decorator);
}

size_t field_node_get_decorator_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_FIELD_FIRST_DECORATOR ? size - CLUX_FIELD_FIRST_DECORATOR
                                           : 0;
}

node_t *field_node_get_decorator(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_FIELD_FIRST_DECORATOR + idx);
}

node_t *create_fields_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_FIELDS);
}

void fields_node_add_field(node_t *self, node_t *field) {
  node_add_child(self, field);
}

size_t fields_node_get_field_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *fields_node_get_field(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}

void field_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "Field");
  node_json_field(writer, "name", field_node_get_name(self), expr_to_json);
  node_json_field(writer, "type", field_node_get_type(self), expr_to_json);
  node_json_children(writer,
                     "decorators",
                     self,
                     CLUX_FIELD_FIRST_DECORATOR,
                     decorator_node_to_json);
  node_json_end(writer);
}

void fields_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "Fields");
  json_writer_key(writer, "fields");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < fields_node_get_field_count(self); i++) {
    field_node_to_json(writer, fields_node_get_field(self, i));
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
