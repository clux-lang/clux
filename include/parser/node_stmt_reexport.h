#ifndef _H_PARSER_NODE_STMT_REEXPORT_
#define _H_PARSER_NODE_STMT_REEXPORT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 重新导出语句 export * from "<path>";（白皮书 §8.4）。
   PATH 为字符串字面量路径（容错时可为 NULL）。 */
#define CLUX_STMT_REEXPORT_PATH 0
#define CLUX_STMT_REEXPORT_CHILDREN_SIZE 1

node_t *create_stmt_reexport_node(allocator_t *allocator);
void stmt_reexport_node_set_path(node_t *self, node_t *path);
node_t *stmt_reexport_node_get_path(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
