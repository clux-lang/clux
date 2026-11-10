#ifndef _H_PARSER_NODE_FUNC_PARAM_
#define _H_PARSER_NODE_FUNC_PARAM_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 单个形参：NAME / TYPE / DEFAULT 均可为 NULL。
   `name: type` 有 NAME；函数类型里的裸 `type` 只有 TYPE。
   只有闭包的捕获列表允许 `= default`（DEFAULT 非 NULL），
   函数参数没有默认值。
   形参列表是 CLUX_NODE_FUNC_PARAMS，children 为 CLUX_NODE_FUNC_PARAM 或 rest。 */
#define CLUX_FUNC_PARAM_NAME 0
#define CLUX_FUNC_PARAM_TYPE 1
#define CLUX_FUNC_PARAM_DEFAULT 2
#define CLUX_FUNC_PARAM_CHILDREN_SIZE 3

node_t *create_func_param_node(allocator_t *allocator);
/* NAME / TYPE / DEFAULT 均允许为 NULL。 */
void func_param_node_set_name(node_t *self, node_t *name);
node_t *func_param_node_get_name(node_t *self);
void func_param_node_set_type(node_t *self, node_t *type);
node_t *func_param_node_get_type(node_t *self);
void func_param_node_set_default(node_t *self, node_t *default_value);
node_t *func_param_node_get_default(node_t *self);

/* 形参列表，children 为 CLUX_NODE_FUNC_PARAM 或 rest 表达式，数量可变。
   函数声明与函数类型共用此节点；函数声明的捕获列表也复用它。 */
node_t *create_func_params_node(allocator_t *allocator);
void func_params_node_add_param(node_t *self, node_t *param);
size_t func_params_node_get_param_count(node_t *self);
/* 越界返回 NULL。 */
node_t *func_params_node_get_param(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
