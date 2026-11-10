#ifndef _H_PARSER_NODE_STMT_FOREACH_
#define _H_PARSER_NODE_STMT_FOREACH_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* foreach 语句 foreach ([var] name [: type]? of iterable) { body }
   （白皮书 §4.9）。
   KEYWORD 为 var 符号（省略时为 NULL，表示只读）；
   NAME 为迭代变量；TYPE 可缺失；ITERABLE 为被迭代表达式；BODY 为块语句。
   除 KEYWORD / TYPE 外，容错时其余槽也可为 NULL。 */
#define CLUX_STMT_FOREACH_KEYWORD 0
#define CLUX_STMT_FOREACH_NAME 1
#define CLUX_STMT_FOREACH_TYPE 2
#define CLUX_STMT_FOREACH_ITERABLE 3
#define CLUX_STMT_FOREACH_BODY 4
#define CLUX_STMT_FOREACH_CHILDREN_SIZE 5

node_t *create_stmt_foreach_node(allocator_t *allocator);
/* KEYWORD 允许为 NULL（未写 var，只读迭代）。 */
void stmt_foreach_node_set_keyword(node_t *self, node_t *keyword);
node_t *stmt_foreach_node_get_keyword(node_t *self);
void stmt_foreach_node_set_name(node_t *self, node_t *name);
node_t *stmt_foreach_node_get_name(node_t *self);
/* TYPE 允许为 NULL（无显式类型标注）。 */
void stmt_foreach_node_set_type(node_t *self, node_t *type);
node_t *stmt_foreach_node_get_type(node_t *self);
void stmt_foreach_node_set_iterable(node_t *self, node_t *iterable);
node_t *stmt_foreach_node_get_iterable(node_t *self);
void stmt_foreach_node_set_body(node_t *self, node_t *body);
node_t *stmt_foreach_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
