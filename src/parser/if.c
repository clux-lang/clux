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

/* 解析 if 链的公共部分。调用前当前 token 必须是 'if'。
   comptime 决定节点种类，并沿 else if 链传递，使整条链都在编译期求值。
   begin_loc 是语句起点（普通 if 为 if 关键字，comptime if 为 comptime）。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_if_chain(allocator_t *allocator,
                             parser_t *parser,
                             bool comptime,
                             location_t begin_loc) {
  parser_next(parser); /* if */
  parser_skip_trivia(parser);

  node_t *node = comptime ? create_stmt_comptime_if_node(allocator)
                          : create_stmt_if_node(allocator);
  node_set_location(node, &begin_loc);

  /* 条件必须带圆括号。 */
  token_t *token = parser_peek_token(parser);
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
    location_t nested_loc = *token_get_location(token);
    else_branch = read_if_chain(allocator, parser, comptime, nested_loc);
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

/* if 语句 if (cond) { then } [else { else } | else if ...]（白皮书 §4.4）。
   调用前当前 token 不一定是 if，不是时返回 NULL 且游标不动。 */
node_t *parser_read_stmt_if(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "if")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  return read_if_chain(allocator, parser, false, begin_loc);
}

/* comptime if 语句 comptime if (cond) { ... }（白皮书 §4.15）。
   条件在编译期求值；else if 链中的嵌套节点同为 comptime if。
   调用前当前 token 不一定是 comptime，不是时返回 NULL 且游标不动。 */
node_t *parser_read_stmt_comptime_if(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "comptime")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* comptime */
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "if")) {
    node_t *node = create_stmt_comptime_if_node(allocator);
    node_set_location(node, &begin_loc);
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected 'if' after 'comptime'"));
    return node;
  }
  return read_if_chain(allocator, parser, true, begin_loc);
}
