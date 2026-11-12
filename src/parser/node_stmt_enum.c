#include "parser/node_stmt_enum.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_stmt_enum_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_ENUM);
  node_resize_children(self, CLUX_STMT_ENUM_FIRST_DECORATOR);
  return self;
}

void stmt_enum_node_set_enum_decl(node_t *self, node_t *enum_decl) {
  node_set_child(self, CLUX_STMT_ENUM_ENUM_DECL, enum_decl);
}

node_t *stmt_enum_node_get_enum_decl(node_t *self) {
  return node_get_child(self, CLUX_STMT_ENUM_ENUM_DECL);
}

void stmt_enum_node_set_export(node_t *self, node_t *export_node) {
  node_set_child(self, CLUX_STMT_ENUM_EXPORT, export_node);
}

node_t *stmt_enum_node_get_export(node_t *self) {
  return node_get_child(self, CLUX_STMT_ENUM_EXPORT);
}

void stmt_enum_node_add_decorator(node_t *self, node_t *decorator) {
  node_add_child(self, decorator);
}

size_t stmt_enum_node_get_decorator_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_STMT_ENUM_FIRST_DECORATOR
             ? size - CLUX_STMT_ENUM_FIRST_DECORATOR
             : 0;
}

node_t *stmt_enum_node_get_decorator(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_STMT_ENUM_FIRST_DECORATOR + idx);
}

void stmt_enum_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtEnum");
  node_json_field(writer,
                  "enum_decl",
                  stmt_enum_node_get_enum_decl(self),
                  enum_decl_node_to_json);
  node_json_field(
      writer, "export", stmt_enum_node_get_export(self), symbol_node_to_json);
  node_json_children(writer,
                     "decorators",
                     self,
                     CLUX_STMT_ENUM_FIRST_DECORATOR,
                     decorator_node_to_json);
  node_json_end(writer);
}
