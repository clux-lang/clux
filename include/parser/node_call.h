#ifndef _H_PARSER_NODE_CALL_
#define _H_PARSER_NODE_CALL_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 函数调用节点：callee(arg, ...)。
   children 长度 = CLUX_EXPR_CALL_FIRST_ARG + 实参个数。
   实参可以是 rest 表达式（含孤立 `...`，由 sema 报错）。 */
#define CLUX_EXPR_CALL_CALLEE 0
#define CLUX_EXPR_CALL_FIRST_ARG 1

/* 创建时只有 CALLEE 槽（无实参）。 */
node_t *create_call_node(allocator_t *allocator);
void call_node_set_callee(node_t *self, node_t *callee);
node_t *call_node_get_callee(node_t *self);
void call_node_add_arg(node_t *self, node_t *arg);
size_t call_node_get_arg_count(node_t *self);
/* 越界返回 NULL。 */
node_t *call_node_get_arg(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
