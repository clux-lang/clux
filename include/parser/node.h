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
  /* 内建类型名（i8..f64、isize/usize、bool/str/void/opaque）。
     语法阶段与 literal 同级，就是普通表达式。 */
  CLUX_NODE_LITERAL_TYPE,

  CLUX_NODE_EXPR_ASSIGN,
  CLUX_NODE_EXPR_TERNARY,
  CLUX_NODE_EXPR_BINARY,
  CLUX_NODE_EXPR_UNARY,
  /* 下标与实例化语法同形（base[expr, ...]），语法阶段无法区分，统一为此类型。 */
  CLUX_NODE_EXPR_SUBSCRIPT,
  /* 切片 base[lo?:hi?]，冒号使其在语法阶段即可区分。 */
  CLUX_NODE_EXPR_SLICE,
  /* rest 表达式 ...expr?，不在优先级链上；孤立 `...` 时无子节点。 */
  CLUX_NODE_EXPR_REST,
  /* 函数调用 callee(arg, ...)；实参可以是 rest 表达式。 */
  CLUX_NODE_EXPR_CALL,
  /* 构造表达式 .type?{ elem, ... }；类型槽可为 NULL（匿名构造），
     元素一律是表达式或 rest 表达式。语法阶段不区分类型与普通表达式。 */
  CLUX_NODE_EXPR_CONSTRUCT,
  /* 括号表达式 ( expr )，只有一个子节点，用于改变结合/优先级。 */
  CLUX_NODE_EXPR_GROUP,
  /* 元组类型 <T, U, ...>；元素数量可变，可含 rest 表达式，也可为空。 */
  CLUX_NODE_EXPR_TUPLE,
  /* 函数定义 / 函数类型 / 闭包，统一为一个节点。
     定义与类型的区分点在参数括号之后：':' 走定义（后接函数体块），
     '->' 走函数类型。语法阶段不做前导校验。 */
  CLUX_NODE_EXPR_FUNC,
  /* 形参列表，children 为 CLUX_NODE_FUNC_PARAM。 */
  CLUX_NODE_FUNC_PARAMS,
  /* 单个形参：NAME / TYPE / DEFAULT 均可为 NULL。 */
  CLUX_NODE_FUNC_PARAM,

  CLUX_NODE_STMT_BLOCK,
  CLUX_NODE_STMT_EXPR,
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
void node_add_child(node_t *self, node_t *child);
node_t *parser_read_symbol(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif