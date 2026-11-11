#ifndef _H_PARSER_NODE_STMT_ENUM_
#define _H_PARSER_NODE_STMT_ENUM_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 枚举语句 <decorator>* [export]? <enum_decl>（白皮书 §3.6、§8.1）。
   ENUM_DECL 槽存枚举声明；EXPORT 为 export 符号或 NULL；其后按源码顺序为
   装饰器，数量可变，因此 children 长度 =
   CLUX_STMT_ENUM_FIRST_DECORATOR + 装饰器个数。 */
#define CLUX_STMT_ENUM_ENUM_DECL 0
#define CLUX_STMT_ENUM_EXPORT 1
#define CLUX_STMT_ENUM_FIRST_DECORATOR 2

node_t *create_stmt_enum_node(allocator_t *allocator);
void stmt_enum_node_set_enum_decl(node_t *self, node_t *enum_decl);
node_t *stmt_enum_node_get_enum_decl(node_t *self);
/* 装饰器按源码顺序追加。 */
/* EXPORT 允许为 NULL（未导出）。 */
void stmt_enum_node_set_export(node_t *self, node_t *export_node);
node_t *stmt_enum_node_get_export(node_t *self);
void stmt_enum_node_add_decorator(node_t *self, node_t *decorator);
size_t stmt_enum_node_get_decorator_count(node_t *self);
/* 越界返回 NULL。 */
node_t *stmt_enum_node_get_decorator(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
