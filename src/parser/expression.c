#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/parser.h"

/* 用首尾子节点的位置拼出表达式自身的区间：起点取 begin->begin，
   终点取 end->end。子节点位置本身不含前后 trivia，因此表达式区间
   也不会携带空白/注释。 */
static void node_set_location_span(node_t *node,
                                   const location_t *begin,
                                   const location_t *end) {
  location_t loc = *begin;
  loc.end = end->end;
  node_set_location(node, &loc);
}

/* 错误位置：优先指向当前未消费的 token；已到流末尾时退化为
   fallback 的终点（零宽区间）。 */
static location_t expression_error_location(parser_t *parser,
                                            const location_t *fallback) {
  token_t *bad = parser_peek_token(parser);
  location_t loc;
  if (bad) {
    loc = *token_get_location(bad);
  } else {
    loc = *fallback;
    loc.begin = fallback->end;
  }
  loc.filename = parser_get_filename(parser);
  return loc;
}

node_t *parser_read_expression_land(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_literal(allocator, parser);
  if (!left) {
    return NULL;
  }
  size_t after_left = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "&&")) {
    /* 不是逻辑与：回退游标到左操作数之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_left);
    return left;
  }
  node_t *expr = create_node(allocator, CLUX_NODE_EXPRESSION_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  size_t after_symbol = parser_get_position(parser);
  node_set_location_span(
      expr, node_get_location(left), node_get_location(symbol));
  parser_skip_trivia(parser);
  /* 递归右结合：a && b && c 解析为 a && (b && c)，与短路求值一致。 */
  node_t *right = parser_read_expression_land(allocator, parser);
  if (!right) {
    /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_set_position(parser, after_symbol);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after '&&'"));
  } else {
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
  }
  return expr;
}
node_t *parser_read_expression_lor(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expression_land(allocator, parser);
  if (!left) {
    return NULL;
  }
  size_t after_left = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "||")) {
    /* 不是逻辑或：回退游标到左操作数之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_left);
    return left;
  }
  node_t *expr = create_node(allocator, CLUX_NODE_EXPRESSION_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  size_t after_symbol = parser_get_position(parser);
  node_set_location_span(
      expr, node_get_location(left), node_get_location(symbol));
  parser_skip_trivia(parser);
  /* 递归右结合：a || b || c 解析为 a || (b || c)，与短路求值一致。 */
  node_t *right = parser_read_expression_lor(allocator, parser);
  if (!right) {
    /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_set_position(parser, after_symbol);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after '||'"));
  } else {
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
  }
  return expr;
}
node_t *parser_read_expression_ternary(allocator_t *allocator,
                                       parser_t *parser) {
  node_t *condition = parser_read_expression_lor(allocator, parser);
  if (!condition) {
    return NULL;
  }
  size_t after_condition = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "?")) {
    /* 不是三元：回退游标到条件之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_condition);
    return condition;
  }
  location_t question_loc = *token_get_location(token);
  parser_next(parser);
  parser_skip_trivia(parser);
  node_t *expr = create_node(allocator, CLUX_NODE_EXPRESSION_TERNARY);
  node_resize_children(expr, CLUX_EXPR_TERNARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_TERNARY_CONDITION, condition);
  node_set_location_span(
      expr, node_get_location(condition), node_get_location(condition));
  node_t *then = parser_read_expression_ternary(allocator, parser);
  if (!then) {
    /* 容错：缺 then 分支仍返回不完整的节点（THEN/ELSE 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    location_t loc = expression_error_location(parser, &question_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after '?'"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_TERNARY_THEN, then);
  node_set_location_span(
      expr, node_get_location(condition), node_get_location(then));
  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, node_get_location(then));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ':' in ternary expression"));
    return expr;
  }
  location_t colon_loc = *token_get_location(token);
  parser_next(parser);
  parser_skip_trivia(parser);
  node_t *else_ = parser_read_expression_ternary(allocator, parser);
  if (!else_) {
    location_t loc = expression_error_location(parser, &colon_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after ':'"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_TERNARY_ELSE, else_);
  node_set_location_span(
      expr, node_get_location(condition), node_get_location(else_));
  return expr;
}
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
  node_t *value = parser_read_expression_ternary(allocator, parser);
  if (!value) {
    /* 容错：缺右值仍返回不完整的节点（VALUE 子节点为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    node_set_location_span(
        expr, node_get_location(id), node_get_location(symbol));
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after assignment"));
  } else {
    node_set_child(expr, CLUX_EXPR_ASSIGN_VALUE, value);
    node_set_location_span(
        expr, node_get_location(id), node_get_location(value));
  }
  return expr;
next:
  return parser_read_expression_ternary(allocator, parser);
}
node_t *parser_read_expression(allocator_t *allocator, parser_t *parser) {
  return parser_read_expression_assign(allocator, parser);
}