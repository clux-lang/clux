#include "parser/node_bitfield_decl.h"
#include "parser/node.h"

node_t *create_bitfield_decl_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_BITFIELD_DECL);
  node_resize_children(self, CLUX_EXPR_BITFIELD_DECL_CHILDREN_SIZE);
  return self;
}

void bitfield_decl_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_EXPR_BITFIELD_DECL_KEYWORD, keyword);
}

node_t *bitfield_decl_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_EXPR_BITFIELD_DECL_KEYWORD);
}

void bitfield_decl_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_BITFIELD_DECL_NAME, name);
}

node_t *bitfield_decl_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_BITFIELD_DECL_NAME);
}

void bitfield_decl_node_set_underlying(node_t *self, node_t *underlying) {
  node_set_child(self, CLUX_EXPR_BITFIELD_DECL_UNDERLYING, underlying);
}

node_t *bitfield_decl_node_get_underlying(node_t *self) {
  return node_get_child(self, CLUX_EXPR_BITFIELD_DECL_UNDERLYING);
}

void bitfield_decl_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_EXPR_BITFIELD_DECL_BODY, body);
}

node_t *bitfield_decl_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_EXPR_BITFIELD_DECL_BODY);
}

void bitfield_decl_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprBitfieldDecl");
  node_json_field(writer,
                  "keyword",
                  bitfield_decl_node_get_keyword(self),
                  symbol_node_to_json);
  node_json_field(
      writer, "name", bitfield_decl_node_get_name(self), expr_to_json);
  node_json_field(writer,
                  "underlying",
                  bitfield_decl_node_get_underlying(self),
                  expr_to_json);
  node_json_field(writer,
                  "body",
                  bitfield_decl_node_get_body(self),
                  bitfield_fields_node_to_json);
  node_json_end(writer);
}
