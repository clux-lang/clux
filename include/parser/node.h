#ifndef _H_PARSER_NODE_
#define _H_PARSER_NODE_
#include "core/allocator.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser.h"
#ifdef __cplusplus
extern "C" {
#endif
enum _node_kind_t {
  CLUX_NODE_SYMBOL,
  CLUX_NODE_PROGRAM,
  CLUX_NODE_STATEMENT_EXPRESSION,
  CLUX_NODE_LITERAL_NIL,
  CLUX_NODE_LITERAL_UNDEFINED,
  CLUX_NODE_LITERAL_STRING,
  CLUX_NODE_LITERAL_CHARACTER,
  CLUX_NODE_LITERAL_NUMERIC,
  CLUX_NODE_LITERAL_BOOLEAN,
  CLUX_NODE_LITERAL_IDENTIFIER,

  CLUX_NODE_EXPRESSION_ASSIGN,
  CLUX_NODE_EXPRESSION_TERNARY,
  CLUX_NODE_EXPRESSION_BINARY,
  CLUX_NODE_EXPRESSION_UNARY,
  /* 下标与实例化语法同形（base[expr, ...]），语法阶段无法区分，统一为此类型。 */
  CLUX_NODE_EXPRESSION_SUBSCRIPT,
  /* 切片 base[lo?:hi?]，冒号使其在语法阶段即可区分。 */
  CLUX_NODE_EXPRESSION_SLICE,
  /* rest 表达式 ...expr?，不在优先级链上；孤立 `...` 时无子节点。 */
  CLUX_NODE_EXPRESSION_REST,
  /* 函数调用 callee(arg, ...)；实参可以是 rest 表达式。 */
  CLUX_NODE_EXPRESSION_CALL,
  /* 构造表达式 .type?{ elem, ... }；类型槽可为 NULL（匿名构造），
     元素一律是表达式或 rest 表达式。语法阶段不区分类型与普通表达式。 */
  CLUX_NODE_EXPRESSION_CONSTRUCT,
};
typedef enum _node_kind_t node_kind_t;
typedef struct _node_t node_t;
node_t *create_node(allocator_t *allocator, node_kind_t kind);
void delete_node(node_t **node);
node_kind_t node_get_kind(node_t *self);
vec_t *node_get_children(node_t *self);
const location_t *node_get_location(node_t *self);
node_t *node_get_parent(node_t *self);
void node_set_parent(node_t *self, node_t *parent);
void node_set_location(node_t *self, const location_t *loc);
void node_resize_children(node_t *self, size_t size);
node_t *node_set_child(node_t *self, size_t idx, node_t *child);
node_t *node_get_child(node_t *self, size_t idx);
node_t *parser_read_symbol(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif