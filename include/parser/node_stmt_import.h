#ifndef _H_PARSER_NODE_STMT_IMPORT_
#define _H_PARSER_NODE_STMT_IMPORT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 导入语句 import <alias> from "<path>";（白皮书 §8.2）。
   ALIAS 为模块别名标识符；PATH 为字符串字面量路径。
   两者在容错时均可为 NULL。 */
#define CLUX_STMT_IMPORT_ALIAS 0
#define CLUX_STMT_IMPORT_PATH 1
#define CLUX_STMT_IMPORT_CHILDREN_SIZE 2

node_t *create_stmt_import_node(allocator_t *allocator);
void stmt_import_node_set_alias(node_t *self, node_t *alias);
node_t *stmt_import_node_get_alias(node_t *self);
void stmt_import_node_set_path(node_t *self, node_t *path);
node_t *stmt_import_node_get_path(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
