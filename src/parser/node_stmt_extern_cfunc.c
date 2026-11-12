#include "parser/node_stmt_extern_cfunc.h"
#include "parser/node.h"

node_t *create_stmt_extern_cfunc_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STMT_EXTERN_CFUNC);
  node_resize_children(self, CLUX_STMT_EXTERN_CFUNC_CHILDREN_SIZE);
  return self;
}

void stmt_extern_cfunc_node_set_export(node_t *self, node_t *export_node) {
  node_set_child(self, CLUX_STMT_EXTERN_CFUNC_EXPORT, export_node);
}

node_t *stmt_extern_cfunc_node_get_export(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_CFUNC_EXPORT);
}

void stmt_extern_cfunc_node_set_lib(node_t *self, node_t *lib) {
  node_set_child(self, CLUX_STMT_EXTERN_CFUNC_LIB, lib);
}

node_t *stmt_extern_cfunc_node_get_lib(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_CFUNC_LIB);
}

void stmt_extern_cfunc_node_set_name(node_t *self, node_t *name) {
  node_set_child(self, CLUX_STMT_EXTERN_CFUNC_NAME, name);
}

node_t *stmt_extern_cfunc_node_get_name(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_CFUNC_NAME);
}

void stmt_extern_cfunc_node_set_params(node_t *self, node_t *params) {
  node_set_child(self, CLUX_STMT_EXTERN_CFUNC_PARAMS, params);
}

node_t *stmt_extern_cfunc_node_get_params(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_CFUNC_PARAMS);
}

void stmt_extern_cfunc_node_set_return(node_t *self, node_t *ret) {
  node_set_child(self, CLUX_STMT_EXTERN_CFUNC_RETURN, ret);
}

node_t *stmt_extern_cfunc_node_get_return(node_t *self) {
  return node_get_child(self, CLUX_STMT_EXTERN_CFUNC_RETURN);
}

void stmt_extern_cfunc_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "StmtExternCfunc");
  node_json_field(writer,
                  "export",
                  stmt_extern_cfunc_node_get_export(self),
                  symbol_node_to_json);
  node_json_field(
      writer, "lib", stmt_extern_cfunc_node_get_lib(self), expr_to_json);
  node_json_field(
      writer, "name", stmt_extern_cfunc_node_get_name(self), expr_to_json);
  node_json_field(writer,
                  "params",
                  stmt_extern_cfunc_node_get_params(self),
                  func_params_node_to_json);
  node_json_field(
      writer, "return", stmt_extern_cfunc_node_get_return(self), expr_to_json);
  node_json_end(writer);
}
