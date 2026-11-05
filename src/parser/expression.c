#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/parser.h"

node_t *parser_read_expression_assign(allocator_t *allocator,
                                      parser_t *parser) {
  size_t position = parser_get_position(parser);
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_IDENTIFIER) {
    goto next;
  }
  parser_next(parser);
  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "+=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "-=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "*=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "/=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "%=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "&=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "|=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "~=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "^=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "&&=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "||=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, ">>=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "<<=")) {
    parser_set_position(parser, position);
    goto next;
  }
  parser_set_position(parser, position);
  node_t *expr = create_node(allocator, CLUX_NODE_EXPRESSION_ASSIGN);
  node_resize_children(expr, CLUX_EXPR_ASSIGN_CHILDREN_SIZE);
  node_t *id = parser_read_literal_identifier(allocator, parser);
  node_set_child(expr, CLUX_EXPR_ASSIGN_ID, id);
  parser_skip_trivia(parser);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_ASSIGN_SYMBOL, symbol);
  parser_skip_trivia(parser);
  node_t *value = parser_read_literal(allocator, parser);
  if (!value) {
    /* 容错：缺右值仍返回不完整的节点（VALUE 子节点为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    location_t loc = {0};
    token_t *bad = parser_peek_token(parser);
    if (bad) {
      loc = *token_get_location(bad);
    } else {
      const location_t *symbol_loc = node_get_location(symbol);
      loc = *symbol_loc;
      loc.begin = symbol_loc->end;
    }
    loc.filename = parser_get_filename(parser);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after assignment"));
  } else {
    node_set_child(expr, CLUX_EXPR_ASSIGN_VALUE, value);
  }
  return expr;
next:
  return parser_read_literal(allocator, parser);
}
node_t *parser_read_expression(allocator_t *allocator, parser_t *parser) {
  return parser_read_expression_assign(allocator, parser);
}