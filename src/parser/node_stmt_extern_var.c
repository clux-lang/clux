#include "parser/node_stmt_extern_var.h"
#include "parser/node.h"

node_t *create_stmt_extern_var_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_EXTERN_VAR);
  node_resize_children(self, CLUX_STMT_EXTERN_VAR_CHILDREN_SIZE);
  return self;
}

void stmt_extern_var_node_set_export(node_t *self, node_t *export_node) {
  node_set_child(self, CLUX_STMT_EXTERN_VAR_EXPORT, export_node);
}

node_t *stmt_extern_var_node_get_export(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_VAR_EXPORT);
}

void stmt_extern_var_node_set_lib(node_t *self, node_t *lib) {
  node_set_child(self, CLUX_STMT_EXTERN_VAR_LIB, lib);
}

node_t *stmt_extern_var_node_get_lib(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_VAR_LIB);
}

void stmt_extern_var_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_STMT_EXTERN_VAR_NAME, name);
}

node_t *stmt_extern_var_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_VAR_NAME);
}

void stmt_extern_var_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_STMT_EXTERN_VAR_TYPE, type);
}

node_t *stmt_extern_var_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_VAR_TYPE);
}

void stmt_extern_var_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtExternVar");
  node_json_field(writer,
                  "export",
                  stmt_extern_var_node_get_export(self),
                  symbol_node_to_json);
  node_json_field(
      writer, "lib", stmt_extern_var_node_get_lib(self), expr_to_json);
  node_json_field(
      writer, "name", stmt_extern_var_node_get_name(self), expr_to_json);
  node_json_field(
      writer, "type", stmt_extern_var_node_get_type(self), expr_to_json);
  node_json_end(writer);
}
