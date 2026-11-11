#ifndef _H_PARSER_NODE_STMT_EXTERN_LIB_
#define _H_PARSER_NODE_STMT_EXTERN_LIB_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 动态库声明 extern lib <alias> from "<path>";（白皮书 §8.8）。
   ALIAS 为库别名标识符；PATH 为字符串字面量（"*" 表示当前进程）。
   两者在容错时均可为 NULL。 */
#define CLUX_STMT_EXTERN_LIB_ALIAS 0
#define CLUX_STMT_EXTERN_LIB_PATH 1
#define CLUX_STMT_EXTERN_LIB_CHILDREN_SIZE 2

node_t *create_stmt_extern_lib_node(allocator_t *allocator);
void stmt_extern_lib_node_set_alias(node_t *self, node_t *alias);
node_t *stmt_extern_lib_node_get_alias(node_t *self);
void stmt_extern_lib_node_set_path(node_t *self, node_t *path);
node_t *stmt_extern_lib_node_get_path(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
