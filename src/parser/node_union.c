#include "parser/node_union.h"
#include "parser/node.h"

node_t *create_union_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_UNION);
  node_resize_children(self, CLUX_EXPR_UNION_CHILDREN_SIZE);
  return self;
}

void union_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_EXPR_UNION_KEYWORD, keyword);
}

node_t *union_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_EXPR_UNION_KEYWORD);
}

void union_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_UNION_NAME, name);
}

node_t *union_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_UNION_NAME);
}

void union_node_set_generics(node_t *self, node_t *generics) {
  node_set_child(self, CLUX_EXPR_UNION_GENERICS, generics);
}

node_t *union_node_get_generics(node_t *self) {
  return node_get_child(self, CLUX_EXPR_UNION_GENERICS);
}

void union_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_EXPR_UNION_BODY, body);
}

node_t *union_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_EXPR_UNION_BODY);
}

void union_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprUnion");
  node_json_field(
      writer, "keyword", union_node_get_keyword(self), symbol_node_to_json);
  node_json_field(writer, "name", union_node_get_name(self), expr_to_json);
  node_json_field(writer,
                  "generics",
                  union_node_get_generics(self),
                  generic_params_node_to_json);
  node_json_field(
      writer, "body", union_node_get_body(self), fields_node_to_json);
  node_json_end(writer);
}
