#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* foreach 语句 foreach ([var] name [: type]? of iterable) { body }
   （白皮书 §4.9）。var 可省略（只读迭代）；类型标注只取后缀层，
   `of` 是关键字，天然终止类型解析。
   调用前当前 token 不一定是 foreach，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_foreach(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "foreach")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* foreach */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_foreach_node(allocator);
  node_set_location(node, &begin_loc);

  /* 头部必须带圆括号。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected '(' after 'foreach'"));
    return node;
  }
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  /* 可选的 var：记录是否可写。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_KEYWORD, "var")) {
    location_t keyword_loc = *token_get_location(token);
    node_t *keyword = create_symbol_node(allocator);
    node_set_location(keyword, &keyword_loc);
    stmt_foreach_node_set_keyword(node, keyword);
    parser_next(parser); /* var */
    parser_skip_trivia(parser);
  }

  /* 迭代变量名。 */
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected iteration variable name in foreach"));
    return node;
  }
  stmt_foreach_node_set_name(node, name);
  node_set_location_span(node, &begin_loc, node_get_location(name));
  parser_skip_trivia(parser);

  /* 可选类型标注 : type。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    parser_next(parser); /* ':' */
    parser_skip_trivia(parser);
    node_t *type = parser_read_expr_postfix(allocator, parser);
    if (!type) {
      location_t loc =
          expression_error_location(parser, node_get_location(name));
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected iteration variable type"));
      return node;
    }
    stmt_foreach_node_set_type(node, type);
    node_set_location_span(node, &begin_loc, node_get_location(type));
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  /* of 引导被迭代表达式。 */
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "of")) {
    location_t loc = expression_error_location(parser, node_get_location(name));
    parser_push_error(
        parser, create_error(allocator, &loc, "expected 'of' in foreach"));
    return node;
  }
  parser_next(parser); /* of */
  parser_skip_trivia(parser);

  node_t *iterable = parser_read_expr(allocator, parser);
  if (!iterable) {
    location_t loc = expression_error_location(parser, node_get_location(name));
    parser_push_error(
        parser, create_error(allocator, &loc, "expected iterable expression"));
    return node;
  }
  stmt_foreach_node_set_iterable(node, iterable);
  node_set_location_span(node, &begin_loc, node_get_location(iterable));
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc =
        expression_error_location(parser, node_get_location(iterable));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' after foreach iterable"));
    return node;
  }
  parser_next(parser); /* ')' */
  parser_skip_trivia(parser);

  /* 循环体必须是块。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc =
        expression_error_location(parser, node_get_location(iterable));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start foreach body"));
    return node;
  }
  node_t *body = parser_read_stmt_block(allocator, parser);
  stmt_foreach_node_set_body(node, body);
  node_set_location_span(node, &begin_loc, node_get_location(body));
  return node;
}
