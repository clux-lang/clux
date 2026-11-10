#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_while.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* while 语句 while (cond) { body }（白皮书 §4.6）。
   条件必须用圆括号包围；循环体必须是块。
   调用前当前 token 不一定是 while，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_while(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "while")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* while */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_while_node(allocator);
  node_set_location(node, &begin_loc);

  /* 条件必须带圆括号。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected '(' after 'while'"));
    return node;
  }
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  node_t *condition = parser_read_expr(allocator, parser);
  if (!condition) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected condition expression"));
    return node;
  }
  stmt_while_node_set_condition(node, condition);
  node_set_location_span(node, &begin_loc, node_get_location(condition));
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc =
        expression_error_location(parser, node_get_location(condition));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' after while condition"));
    return node;
  }
  parser_next(parser); /* ')' */
  parser_skip_trivia(parser);

  /* 循环体必须是块。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc =
        expression_error_location(parser, node_get_location(condition));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start while body"));
    return node;
  }
  node_t *body = parser_read_stmt_block(allocator, parser);
  stmt_while_node_set_body(node, body);
  node_set_location_span(node, &begin_loc, node_get_location(body));
  return node;
}
