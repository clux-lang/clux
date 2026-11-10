#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_if.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* if 语句 if (cond) { then } [else { else } | else if ...]（白皮书 §4.4）。
   条件必须用圆括号包围；else if 递归为嵌套的 if 语句。
   调用前当前 token 不一定是 if，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_if(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "if")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* if */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_if_node(allocator);
  node_set_location(node, &begin_loc);

  /* 条件必须带圆括号。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(parser,
                      create_error(allocator, &loc, "expected '(' after 'if'"));
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
  stmt_if_node_set_condition(node, condition);
  node_set_location_span(node, &begin_loc, node_get_location(condition));
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc =
        expression_error_location(parser, node_get_location(condition));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' after if condition"));
    return node;
  }
  parser_next(parser); /* ')' */
  parser_skip_trivia(parser);

  /* then 分支必须是块。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc =
        expression_error_location(parser, node_get_location(condition));
    parser_push_error(
        parser, create_error(allocator, &loc, "expected '{' to start if body"));
    return node;
  }
  node_t *then_branch = parser_read_stmt_block(allocator, parser);
  stmt_if_node_set_then(node, then_branch);
  node_set_location_span(node, &begin_loc, node_get_location(then_branch));
  parser_skip_trivia(parser);

  /* 可选 else：块或另一个 if（else if 链）。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "else")) {
    return node;
  }
  parser_next(parser); /* else */
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  node_t *else_branch = NULL;
  if (token_is(token, CLUX_TOKEN_KEYWORD, "if")) {
    else_branch = parser_read_stmt_if(allocator, parser);
  } else if (token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    else_branch = parser_read_stmt_block(allocator, parser);
  } else {
    location_t loc =
        expression_error_location(parser, node_get_location(then_branch));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' or 'if' after 'else'"));
  }
  if (else_branch) {
    stmt_if_node_set_else(node, else_branch);
    node_set_location_span(node, &begin_loc, node_get_location(else_branch));
  }
  return node;
}
