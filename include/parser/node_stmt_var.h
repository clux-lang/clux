#ifndef _H_PARSER_NODE_STMT_VAR_
#define _H_PARSER_NODE_STMT_VAR_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 变量定义语句 <decorator>* [comptime]? (var|using) name [: type]? = expr;
   （白皮书 §4.1、§3.4、§4.12.1）。
   KEYWORD 区分 var 与 using；NAME 必有；TYPE 可缺失（由初值推断）；
   VALUE 为初值表达式，必有；COMPTIME 为 comptime 符号或 NULL；
   其后按源码顺序为装饰器，数量可变，
   因此 children 长度 = CLUX_STMT_VAR_FIRST_DECORATOR + 装饰器个数。
   `comptime using` 语法上可解析，是否合法由语义阶段检查。 */
#define CLUX_STMT_VAR_KEYWORD 0
#define CLUX_STMT_VAR_NAME 1
#define CLUX_STMT_VAR_TYPE 2
#define CLUX_STMT_VAR_VALUE 3
#define CLUX_STMT_VAR_COMPTIME 4
#define CLUX_STMT_VAR_FIRST_DECORATOR 5

node_t *create_stmt_var_node(allocator_t *allocator);
void stmt_var_node_set_keyword(node_t *self, node_t *keyword);
node_t *stmt_var_node_get_keyword(node_t *self);
void stmt_var_node_set_name(node_t *self, node_t *name);
node_t *stmt_var_node_get_name(node_t *self);
/* TYPE 允许为 NULL（由初值推断）。 */
void stmt_var_node_set_type(node_t *self, node_t *type);
node_t *stmt_var_node_get_type(node_t *self);
void stmt_var_node_set_value(node_t *self, node_t *value);
node_t *stmt_var_node_get_value(node_t *self);
/* COMPTIME 允许为 NULL（普通变量语句）。 */
void stmt_var_node_set_comptime(node_t *self, node_t *comptime);
node_t *stmt_var_node_get_comptime(node_t *self);
void stmt_var_node_add_decorator(node_t *self, node_t *decorator);
size_t stmt_var_node_get_decorator_count(node_t *self);
/* 越界返回 NULL。 */
node_t *stmt_var_node_get_decorator(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
