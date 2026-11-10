#ifndef _H_PARSER_NODE_SLICE_
#define _H_PARSER_NODE_SLICE_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 切片节点：base[lo?:hi?]，LO/HI 可为 NULL（省略该端）。 */
#define CLUX_EXPR_SLICE_CHILDREN_SIZE 3
#define CLUX_EXPR_SLICE_BASE 0
#define CLUX_EXPR_SLICE_LO 1
#define CLUX_EXPR_SLICE_HI 2

node_t *create_slice_node(allocator_t *allocator);
void slice_node_set_base(node_t *self, node_t *base);
node_t *slice_node_get_base(node_t *self);
/* LO/HI 允许为 NULL。 */
void slice_node_set_lo(node_t *self, node_t *lo);
node_t *slice_node_get_lo(node_t *self);
void slice_node_set_hi(node_t *self, node_t *hi);
node_t *slice_node_get_hi(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
