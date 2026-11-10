#ifndef _H_PARSER_NODE_SUBSCRIPT_
#define _H_PARSER_NODE_SUBSCRIPT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 下标/实例化节点：base[arg, ...]。
   语法阶段无法区分下标与实例化（两者同形），由 sema 按 BASE 的类型判定。
   children 长度 = CLUX_EXPR_SUBSCRIPT_FIRST_ARG + 实参个数。 */
#define CLUX_EXPR_SUBSCRIPT_BASE 0
#define CLUX_EXPR_SUBSCRIPT_FIRST_ARG 1

/* 创建时只有 BASE 槽（无实参）。 */
node_t *create_subscript_node(allocator_t *allocator);
void subscript_node_set_base(node_t *self, node_t *base);
node_t *subscript_node_get_base(node_t *self);
/* 追加一个实参（下标或实例化参数）。 */
void subscript_node_add_arg(node_t *self, node_t *arg);
size_t subscript_node_get_arg_count(node_t *self);
/* 越界返回 NULL。 */
node_t *subscript_node_get_arg(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
