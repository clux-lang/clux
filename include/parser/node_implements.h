#ifndef _H_PARSER_NODE_IMPLEMENTS_
#define _H_PARSER_NODE_IMPLEMENTS_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* implements 列表 implements I[S] & J[S]（白皮书 §3.5）。
   children 按源码顺序为接口类型表达式。 */
node_t *create_implements_node(allocator_t *allocator);
void implements_node_add_type(node_t *self, node_t *type);
size_t implements_node_get_type_count(node_t *self);
/* 越界返回 NULL。 */
node_t *implements_node_get_type(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
