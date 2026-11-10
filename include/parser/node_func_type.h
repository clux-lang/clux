#ifndef _H_PARSER_NODE_FUNC_TYPE_
#define _H_PARSER_NODE_FUNC_TYPE_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 函数类型 func |captures|(params) -> ret（白皮书 §2.7）。
   CAPTURES 为捕获列表（无捕获时为 NULL），PARAMS 为形参类型列表，
   RETURN 为返回类型。形参是裸类型表达式，不是 FUNC_PARAM。 */
#define CLUX_EXPR_FUNC_TYPE_CAPTURES 0
#define CLUX_EXPR_FUNC_TYPE_PARAMS 1
#define CLUX_EXPR_FUNC_TYPE_RETURN 2
#define CLUX_EXPR_FUNC_TYPE_CHILDREN_SIZE 3

node_t *create_func_type_node(allocator_t *allocator);
/* CAPTURES 允许为 NULL（无捕获）。 */
void func_type_node_set_captures(node_t *self, node_t *captures);
node_t *func_type_node_get_captures(node_t *self);
void func_type_node_set_params(node_t *self, node_t *params);
node_t *func_type_node_get_params(node_t *self);
/* RETURN 允许为 NULL（容错：缺返回类型）。 */
void func_type_node_set_return(node_t *self, node_t *ret);
node_t *func_type_node_get_return(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
