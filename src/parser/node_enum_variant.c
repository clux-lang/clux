#include "parser/node_enum_variant.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_enum_variant_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_ENUM_VARIANT);
  node_resize_children(self, CLUX_ENUM_VARIANT_CHILDREN_SIZE);
  return self;
}

void enum_variant_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_ENUM_VARIANT_NAME, name);
}

node_t *enum_variant_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_ENUM_VARIANT_NAME);
}

void enum_variant_node_set_value(node_t *self, node_t *value) {
  node_set_child(self, CLUX_ENUM_VARIANT_VALUE, value);
}

node_t *enum_variant_node_get_value(node_t *self) {
  return node_get_child(self, CLUX_ENUM_VARIANT_VALUE);
}

node_t *create_enum_variants_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_ENUM_VARIANTS);
}

void enum_variants_node_add_variant(node_t *self, node_t *variant) {
  node_add_child(self, variant);
}

size_t enum_variants_node_get_variant_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *enum_variants_node_get_variant(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}

void enum_variant_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "EnumVariant");
  node_json_field(
      writer, "name", enum_variant_node_get_name(self), expr_to_json);
  node_json_field(
      writer, "value", enum_variant_node_get_value(self), expr_to_json);
  node_json_end(writer);
}

void enum_variants_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "EnumVariants");
  json_writer_key(writer, "variants");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < enum_variants_node_get_variant_count(self); i++) {
    enum_variant_node_to_json(writer, enum_variants_node_get_variant(self, i));
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}
