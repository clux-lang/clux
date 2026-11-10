#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/decorator.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_field.h"
#include "parser/parser.h"

/* 单个字段 name: type;（白皮书 §3.5）。struct / tag union / cunion 共用。
   allow_decorators 为 true 时先读取前导装饰器（struct 字段允许；
   tag union / cunion 不允许，但该限制由调用方通过 false 表达）。
   调用前当前 token 不一定是标识符或装饰器，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_field(allocator_t *allocator,
                          parser_t *parser,
                          bool allow_decorators) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = allow_decorators && parser_peek_decorator(parser);
  if (!has_decorators &&
      (!token || token_get_kind(token) != CLUX_TOKEN_IDENTIFIER)) {
    return NULL;
  }
  location_t begin_loc;
  if (has_decorators) {
    begin_loc = *token_get_location(token);
  }

  node_t *node = create_field_node(allocator);
  if (has_decorators) {
    while (parser_peek_decorator(parser)) {
      field_node_add_decorator(node, parser_read_decorator(allocator, parser));
      parser_skip_trivia(parser);
    }
  }

  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    if (!has_decorators) {
      /* 不是字段起始：游标不动，交回调用方。 */
      delete_node(&node);
      return NULL;
    }
    /* 装饰器后不是字段名：保留装饰器，由调用方报错。 */
    node_set_location(node, &begin_loc);
    return node;
  }
  location_t name_loc = *node_get_location(name);
  if (!has_decorators) {
    begin_loc = name_loc;
  }
  field_node_set_name(node, name);
  node_set_location_span(node, &begin_loc, &name_loc);

  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected ':' after field name"));
    return node;
  }
  parser_next(parser); /* ':' */
  parser_skip_trivia(parser);
  node_t *type = parser_read_expr(allocator, parser);
  if (!type) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(parser,
                      create_error(allocator, &loc, "expected field type"));
    return node;
  }
  field_node_set_type(node, type);
  node_set_location_span(node, &begin_loc, node_get_location(type));
  return node;
}
