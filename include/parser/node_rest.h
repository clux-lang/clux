#ifndef _H_PARSER_NODE_REST_
#define _H_PARSER_NODE_REST_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* rest 表达式 ...expr?，不在优先级链上。
   有操作数时 children = [expr]（长度 1），孤立 `...` 时长度 0。 */
#define CLUX_EXPR_REST_EXPR 0

/* 创建孤立 rest 节点（无操作数，children 为空）。 */
node_t *create_rest_node(allocator_t *allocator);
/* 设置操作数：首次调用时把 children 扩到 1 槽。 */
void rest_node_set_expr(node_t *self, node_t *expr);
/* 孤立 `...` 时返回 NULL。 */
node_t *rest_node_get_expr(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
