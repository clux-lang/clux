#ifndef _H_PARSER_NODE_GENERIC_PARAM_
#define _H_PARSER_NODE_GENERIC_PARAM_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 单个泛型参数，按白皮书 §6.1–§6.5 的写法：
     T                      —— 类型参数
     N: comptime usize      —— 编译期值参数（TYPE 非 NULL）
     W extends IWriter[W]   —— 约束（CONSTRAINT 非 NULL）
     T = i32                —— 默认值（DEFAULT 非 NULL）
   NAME 必有；其余槽位按需出现，顺序为 TYPE → CONSTRAINT → DEFAULT。
   泛型参数列表是 CLUX_NODE_GENERIC_PARAMS，children 为
   CLUX_NODE_GENERIC_PARAM 或 rest。 */
#define CLUX_GENERIC_PARAM_NAME 0
#define CLUX_GENERIC_PARAM_TYPE 1
#define CLUX_GENERIC_PARAM_CONSTRAINT 2
#define CLUX_GENERIC_PARAM_DEFAULT 3
#define CLUX_GENERIC_PARAM_CHILDREN_SIZE 4

node_t *create_generic_param_node(allocator_t *allocator);
void generic_param_node_set_name(node_t *self, node_t *name);
node_t *generic_param_node_get_name(node_t *self);
/* TYPE / CONSTRAINT / DEFAULT 均允许为 NULL。 */
void generic_param_node_set_type(node_t *self, node_t *type);
node_t *generic_param_node_get_type(node_t *self);
void generic_param_node_set_constraint(node_t *self, node_t *constraint);
node_t *generic_param_node_get_constraint(node_t *self);
void generic_param_node_set_default(node_t *self, node_t *default_value);
node_t *generic_param_node_get_default(node_t *self);

/* 泛型参数列表 [T, U, ...]，children 为 CLUX_NODE_GENERIC_PARAM 或 rest。 */
node_t *create_generic_params_node(allocator_t *allocator);
void generic_params_node_add_param(node_t *self, node_t *param);
size_t generic_params_node_get_param_count(node_t *self);
/* 越界返回 NULL。 */
node_t *generic_params_node_get_param(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
