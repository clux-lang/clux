#include "parser/node_bitfield_field.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_bitfield_field_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_BITFIELD_FIELD);
  node_resize_children(self, CLUX_BITFIELD_FIELD_CHILDREN_SIZE);
  return self;
}

void bitfield_field_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_BITFIELD_FIELD_NAME, name);
}

node_t *bitfield_field_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_BITFIELD_FIELD_NAME);
}

void bitfield_field_node_set_width(node_t *self, node_t *width) {
  node_set_child(self, CLUX_BITFIELD_FIELD_WIDTH, width);
}

node_t *bitfield_field_node_get_width(node_t *self) {
  return node_get_child(self, CLUX_BITFIELD_FIELD_WIDTH);
}

node_t *create_bitfield_fields_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_BITFIELD_FIELDS);
}

void bitfield_fields_node_add_field(node_t *self, node_t *field) {
  node_add_child(self, field);
}

size_t bitfield_fields_node_get_field_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *bitfield_fields_node_get_field(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}

void bitfield_field_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "BitfieldField");
  node_json_field(
      writer, "name", bitfield_field_node_get_name(self), expr_to_json);
  node_json_field(
      writer, "width", bitfield_field_node_get_width(self), expr_to_json);
  node_json_end(writer);
}

void bitfield_fields_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "BitfieldFields");
  json_writer_key(writer, "fields");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < bitfield_fields_node_get_field_count(self); i++) {
    bitfield_field_node_to_json(writer,
                                bitfield_fields_node_get_field(self, i));
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
