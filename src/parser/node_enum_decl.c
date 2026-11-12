#include "parser/node_enum_decl.h"
#include "parser/node.h"

node_t *create_enum_decl_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_ENUM_DECL);
  node_resize_children(self, CLUX_EXPR_ENUM_DECL_CHILDREN_SIZE);
  return self;
}

void enum_decl_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_EXPR_ENUM_DECL_KEYWORD, keyword);
}

node_t *enum_decl_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_EXPR_ENUM_DECL_KEYWORD);
}

void enum_decl_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_ENUM_DECL_NAME, name);
}

node_t *enum_decl_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_ENUM_DECL_NAME);
}

void enum_decl_node_set_underlying(node_t *self, node_t *underlying) {
  node_set_child(self, CLUX_EXPR_ENUM_DECL_UNDERLYING, underlying);
}

node_t *enum_decl_node_get_underlying(node_t *self) {
  return node_get_child(self, CLUX_EXPR_ENUM_DECL_UNDERLYING);
}

void enum_decl_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_EXPR_ENUM_DECL_BODY, body);
}

node_t *enum_decl_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_EXPR_ENUM_DECL_BODY);
}

void enum_decl_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprEnumDecl");
  node_json_field(
      writer, "keyword", enum_decl_node_get_keyword(self), symbol_node_to_json);
  node_json_field(writer, "name", enum_decl_node_get_name(self), expr_to_json);
  node_json_field(
      writer, "underlying", enum_decl_node_get_underlying(self), expr_to_json);
  node_json_field(writer,
                  "body",
                  enum_decl_node_get_body(self),
                  enum_variants_node_to_json);
  node_json_end(writer);
}
