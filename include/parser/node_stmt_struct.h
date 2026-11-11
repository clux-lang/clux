#ifndef _H_PARSER_NODE_STMT_STRUCT_
#define _H_PARSER_NODE_STMT_STRUCT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 结构体语句 <decorator>* [export]? <struct_decl>（白皮书 §3.5、§8.1）。
   STRUCT_DECL 槽存结构体声明；EXPORT 为 export 符号或 NULL；其后按源码
   顺序为装饰器，数量可变，因此 children 长度 =
   CLUX_STMT_STRUCT_FIRST_DECORATOR + 装饰器个数。 */
#define CLUX_STMT_STRUCT_STRUCT_DECL 0
#define CLUX_STMT_STRUCT_EXPORT 1
#define CLUX_STMT_STRUCT_FIRST_DECORATOR 2

node_t *create_stmt_struct_node(allocator_t *allocator);
void stmt_struct_node_set_struct_decl(node_t *self, node_t *struct_decl);
node_t *stmt_struct_node_get_struct_decl(node_t *self);
/* 装饰器按源码顺序追加。 */
/* EXPORT 允许为 NULL（未导出）。 */
void stmt_struct_node_set_export(node_t *self, node_t *export_node);
node_t *stmt_struct_node_get_export(node_t *self);
void stmt_struct_node_add_decorator(node_t *self, node_t *decorator);
size_t stmt_struct_node_get_decorator_count(node_t *self);
/* 越界返回 NULL。 */
node_t *stmt_struct_node_get_decorator(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
