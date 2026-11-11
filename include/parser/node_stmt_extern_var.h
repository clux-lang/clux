#ifndef _H_PARSER_NODE_STMT_EXTERN_VAR_
#define _H_PARSER_NODE_STMT_EXTERN_VAR_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 外部变量声明 [export]? extern var <lib>::<name>: type;
   （白皮书 §8.10）。EXPORT 可缺失；LIB / NAME 为限定名的两段；
   TYPE 为变量类型。容错时各槽均可为 NULL。 */
#define CLUX_STMT_EXTERN_VAR_EXPORT 0
#define CLUX_STMT_EXTERN_VAR_LIB 1
#define CLUX_STMT_EXTERN_VAR_NAME 2
#define CLUX_STMT_EXTERN_VAR_TYPE 3
#define CLUX_STMT_EXTERN_VAR_CHILDREN_SIZE 4

node_t *create_stmt_extern_var_node(allocator_t *allocator);
void stmt_extern_var_node_set_export(node_t *self, node_t *export_node);
node_t *stmt_extern_var_node_get_export(node_t *self);
void stmt_extern_var_node_set_lib(node_t *self, node_t *lib);
node_t *stmt_extern_var_node_get_lib(node_t *self);
void stmt_extern_var_node_set_name(node_t *self, node_t *name);
node_t *stmt_extern_var_node_get_name(node_t *self);
void stmt_extern_var_node_set_type(node_t *self, node_t *type);
node_t *stmt_extern_var_node_get_type(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
