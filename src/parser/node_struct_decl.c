#include "parser/node_struct_member.h"
#include "parser/node_struct_decl.h"
#include "parser/node.h"

node_t *create_struct_decl_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_EXPR_STRUCT_DECL);
  node_resize_children(self, CLUX_EXPR_STRUCT_DECL_CHILDREN_SIZE);
  return self;
}

void struct_decl_node_set_keyword(node_t *self, node_t *keyword) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_KEYWORD, keyword);
}

node_t *struct_decl_node_get_keyword(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_KEYWORD);
}

void struct_decl_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_NAME, name);
}

node_t *struct_decl_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_NAME);
}

void struct_decl_node_set_generics(node_t *self, node_t *generics) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_GENERICS, generics);
}

node_t *struct_decl_node_get_generics(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_GENERICS);
}

void struct_decl_node_set_implements(node_t *self, node_t *implements) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_IMPLEMENTS, implements);
}

node_t *struct_decl_node_get_implements(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_IMPLEMENTS);
}

void struct_decl_node_set_body(node_t *self, node_t *body) {
  node_set_child(self, CLUX_EXPR_STRUCT_DECL_BODY, body);
}

node_t *struct_decl_node_get_body(node_t *self) {
  return node_get_child(self, CLUX_EXPR_STRUCT_DECL_BODY);
}

void struct_decl_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "ExprStructDecl");
  node_json_field(writer,
                  "keyword",
                  struct_decl_node_get_keyword(self),
                  symbol_node_to_json);
  node_json_field(
      writer, "name", struct_decl_node_get_name(self), expr_to_json);
  node_json_field(writer,
                  "generics",
                  struct_decl_node_get_generics(self),
                  generic_params_node_to_json);
  node_json_field(writer,
                  "implements",
                  struct_decl_node_get_implements(self),
                  implements_node_to_json);
  node_json_field(writer,
                  "body",
                  struct_decl_node_get_body(self),
                  struct_members_node_to_json);
  node_json_end(writer);
}

void struct_members_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StructMembers");
  json_writer_key(writer, "members");
  json_writer_begin_array(writer);
  for (size_t i = 0; i < struct_members_node_get_member_count(self); i++) {
    node_t *member = struct_members_node_get_member(self, i);
    switch (node_get_kind(member)) {
    case CLUX_NODE_FIELD:
      field_node_to_json(writer, member);
      break;
    case CLUX_NODE_STMT_VAR:
      stmt_var_node_to_json(writer, member);
      break;
    case CLUX_NODE_STMT_FUNC:
      stmt_func_node_to_json(writer, member);
      break;
    case CLUX_NODE_STRUCT_EMBED:
      struct_embed_node_to_json(writer, member);
      break;
    default:
      json_writer_null(writer);
      break;
    }
  }
  json_writer_end_array(writer);
  node_json_end(writer);
}

void struct_embed_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StructEmbed");
  node_json_field(
      writer, "type", struct_embed_node_get_type(self), expr_to_json);
  node_json_end(writer);
}
