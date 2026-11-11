#ifndef _H_PARSER_NODE_STMT_EXTERN_CFUNC_
#define _H_PARSER_NODE_STMT_EXTERN_CFUNC_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 外部 C 函数声明 [export]? extern cfunc <lib>::<name>(params): ret;
   （白皮书 §8.9）。没有函数体，符号由运行时动态解析。
   EXPORT 可缺失；LIB / NAME 为限定名的两段；PARAMS 为形参列表；
   RETURN 为返回类型。容错时各槽均可为 NULL。 */
#define CLUX_STMT_EXTERN_CFUNC_EXPORT 0
#define CLUX_STMT_EXTERN_CFUNC_LIB 1
#define CLUX_STMT_EXTERN_CFUNC_NAME 2
#define CLUX_STMT_EXTERN_CFUNC_PARAMS 3
#define CLUX_STMT_EXTERN_CFUNC_RETURN 4
#define CLUX_STMT_EXTERN_CFUNC_CHILDREN_SIZE 5

node_t *create_stmt_extern_cfunc_node(allocator_t *allocator);
void stmt_extern_cfunc_node_set_export(node_t *self, node_t *export_node);
node_t *stmt_extern_cfunc_node_get_export(node_t *self);
void stmt_extern_cfunc_node_set_lib(node_t *self, node_t *lib);
node_t *stmt_extern_cfunc_node_get_lib(node_t *self);
void stmt_extern_cfunc_node_set_name(node_t *self, node_t *name);
node_t *stmt_extern_cfunc_node_get_name(node_t *self);
void stmt_extern_cfunc_node_set_params(node_t *self, node_t *params);
node_t *stmt_extern_cfunc_node_get_params(node_t *self);
void stmt_extern_cfunc_node_set_return(node_t *self, node_t *ret);
node_t *stmt_extern_cfunc_node_get_return(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
