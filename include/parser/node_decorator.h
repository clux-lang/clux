#ifndef _H_PARSER_NODE_DECORATOR_
#define _H_PARSER_NODE_DECORATOR_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 装饰器节点 [[ <expr> ]]（白皮书 §3.2、§9.1）。
   不在主节点树上：由顶层声明、struct 字段/方法等特定语句主动调用，
   作为它们的子节点挂载。children 只有 EXPR 一个槽。 */
#define CLUX_DECORATOR_EXPR 0
#define CLUX_DECORATOR_CHILDREN_SIZE 1

node_t *create_decorator_node(allocator_t *allocator);
/* EXPR 允许为 NULL（容错：缺装饰器表达式）。 */
void decorator_node_set_expr(node_t *self, node_t *expr);
node_t *decorator_node_get_expr(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
