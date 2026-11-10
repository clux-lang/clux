#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_for.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* for 语句 for (init; cond; update) { body }（白皮书 §4.8）。
   初始化可以是变量语句或表达式，三段均可省略；循环体必须是块。
   调用前当前 token 不一定是 for，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_for(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "for")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* for */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_for_node(allocator);
  node_set_location(node, &begin_loc);

  /* 头部必须带圆括号。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected '(' after 'for'"));
    return node;
  }
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  /* 初始化：省略 / 变量语句（自带 ';'）/ 表达式。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    parser_next(parser);
    parser_skip_trivia(parser);
  } else if (token_is(token, CLUX_TOKEN_KEYWORD, "var") ||
             token_is(token, CLUX_TOKEN_KEYWORD, "using")) {
    node_t *init = parser_read_stmt_var(allocator, parser);
    stmt_for_node_set_init(node, init);
    parser_skip_trivia(parser);
  } else {
    node_t *init = parser_read_expr(allocator, parser);
    if (!init) {
      location_t loc = expression_error_location(parser, &begin_loc);
      parser_push_error(
          parser,
          create_error(allocator,
                       &loc,
                       "expected variable declaration or expression in for "
                       "initializer"));
      return node;
    }
    stmt_for_node_set_init(node, init);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
      location_t loc =
          expression_error_location(parser, node_get_location(init));
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected ';' after for initializer"));
      return node;
    }
    parser_next(parser); /* ';' */
    parser_skip_trivia(parser);
  }

  /* 条件（可省略）。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    node_t *condition = parser_read_expr(allocator, parser);
    if (!condition) {
      location_t loc = expression_error_location(parser, &begin_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected condition expression"));
      return node;
    }
    stmt_for_node_set_condition(node, condition);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
      location_t loc =
          expression_error_location(parser, node_get_location(condition));
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected ';' after for condition"));
      return node;
    }
  }
  parser_next(parser); /* ';' */
  parser_skip_trivia(parser);

  /* 更新（可省略）。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    node_t *update = parser_read_expr(allocator, parser);
    if (!update) {
      location_t loc = expression_error_location(parser, &begin_loc);
      parser_push_error(
          parser, create_error(allocator, &loc, "expected update expression"));
      return node;
    }
    stmt_for_node_set_update(node, update);
    parser_skip_trivia(parser);
  }
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected ')' after for update"));
    return node;
  }
  parser_next(parser); /* ')' */
  parser_skip_trivia(parser);

  /* 循环体必须是块。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start for body"));
    return node;
  }
  node_t *body = parser_read_stmt_block(allocator, parser);
  stmt_for_node_set_body(node, body);
  node_set_location_span(node, &begin_loc, node_get_location(body));
  return node;
}
