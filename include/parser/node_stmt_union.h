#ifndef _H_PARSER_NODE_STMT_UNION_
#define _H_PARSER_NODE_STMT_UNION_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* union 语句 <decorator>* [export]? <union_decl>（白皮书 §3.7、§3.8、§8.1）。
   UNION_DECL 槽存 union/cunion 声明；EXPORT 为 export 符号或 NULL；
   其后按源码顺序为装饰器，数量可变，因此 children 长度 =
   CLUX_STMT_UNION_FIRST_DECORATOR + 装饰器个数。 */
#define CLUX_STMT_UNION_UNION_DECL 0
#define CLUX_STMT_UNION_EXPORT 1
#define CLUX_STMT_UNION_FIRST_DECORATOR 2

node_t *create_stmt_union_node(allocator_t *allocator);
void stmt_union_node_set_union_decl(node_t *self, node_t *union_decl);
node_t *stmt_union_node_get_union_decl(node_t *self);
/* 装饰器按源码顺序追加。 */
/* EXPORT 允许为 NULL（未导出）。 */
void stmt_union_node_set_export(node_t *self, node_t *export_node);
node_t *stmt_union_node_get_export(node_t *self);
void stmt_union_node_add_decorator(node_t *self, node_t *decorator);
size_t stmt_union_node_get_decorator_count(node_t *self);
/* 越界返回 NULL。 */
node_t *stmt_union_node_get_decorator(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
