#include "parser/node_stmt_interface.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_stmt_interface_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_INTERFACE);
  node_resize_children(self, CLUX_STMT_INTERFACE_FIRST_DECORATOR);
  return self;
}

void stmt_interface_node_set_interface_decl(node_t *self, node_t *decl) {
  node_set_child(self, CLUX_STMT_INTERFACE_INTERFACE_DECL, decl);
}

node_t *stmt_interface_node_get_interface_decl(node_t *self) {
  return node_get_child(self, CLUX_STMT_INTERFACE_INTERFACE_DECL);
}

void stmt_interface_node_set_export(node_t *self, node_t *export_node) {
  node_set_child(self, CLUX_STMT_INTERFACE_EXPORT, export_node);
}

node_t *stmt_interface_node_get_export(node_t *self) {
  return node_get_child(self, CLUX_STMT_INTERFACE_EXPORT);
}

void stmt_interface_node_add_decorator(node_t *self, node_t *decorator) {
  node_add_child(self, decorator);
}

size_t stmt_interface_node_get_decorator_count(node_t *self) {
  size_t size = vec_get_size(node_get_children(self));
  return size > CLUX_STMT_INTERFACE_FIRST_DECORATOR
             ? size - CLUX_STMT_INTERFACE_FIRST_DECORATOR
             : 0;
}

node_t *stmt_interface_node_get_decorator(node_t *self, size_t idx) {
  return node_get_child(self, CLUX_STMT_INTERFACE_FIRST_DECORATOR + idx);
}

void stmt_interface_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtInterface");
  node_json_field(writer,
                  "interface_decl",
                  stmt_interface_node_get_interface_decl(self),
                  interface_decl_node_to_json);
  node_json_field(writer,
                  "export",
                  stmt_interface_node_get_export(self),
                  symbol_node_to_json);
  node_json_children(writer,
                     "decorators",
                     self,
                     CLUX_STMT_INTERFACE_FIRST_DECORATOR,
                     decorator_node_to_json);
  node_json_end(writer);
}
