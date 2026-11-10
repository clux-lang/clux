#ifndef _H_PARSER_NODE_UNION_
#define _H_PARSER_NODE_UNION_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* union 定义（白皮书 §3.7 tag union、§3.8 C union）。
   tag union 与 cunion 在语法阶段完全相同，只有关键词不同，因此共用此节点：
   KEYWORD 为 `union` 或 `cunion` 符号，语义差异（是否带 tag）留给语义阶段；
   NAME 为类型名；GENERICS 为泛型参数列表（无泛型时为 NULL）；
   BODY 为字段列表节点，children 为 CLUX_NODE_FIELD。 */
#define CLUX_EXPR_UNION_KEYWORD 0
#define CLUX_EXPR_UNION_NAME 1
#define CLUX_EXPR_UNION_GENERICS 2
#define CLUX_EXPR_UNION_BODY 3
#define CLUX_EXPR_UNION_CHILDREN_SIZE 4

node_t *create_union_node(allocator_t *allocator);
void union_node_set_keyword(node_t *self, node_t *keyword);
node_t *union_node_get_keyword(node_t *self);
void union_node_set_name(node_t *self, node_t *name);
node_t *union_node_get_name(node_t *self);
/* GENERICS 允许为 NULL（无泛型）。 */
void union_node_set_generics(node_t *self, node_t *generics);
node_t *union_node_get_generics(node_t *self);
/* BODY 为 CLUX_NODE_FIELDS；容错时可为 NULL。 */
void union_node_set_body(node_t *self, node_t *body);
node_t *union_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
