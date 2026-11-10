#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_param.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* 单个 defer 捕获项 name [: type]? [= expr]?（白皮书 §4.12）。
   复用 CLUX_NODE_FUNC_PARAM：NAME 必有，TYPE / DEFAULT 可缺失。
   parenthesized 为 true 时默认值可用完整表达式（')' 天然终止）；
   裸列表形式的默认值停在 '|' 之前，因此只取到异或层，避免把闭合 '|'
   当作位或运算符吃掉。
   调用前当前 token 不一定是标识符，不是时返回 NULL 且游标不动。 */
static node_t *read_capture_item(allocator_t *allocator,
                                 parser_t *parser,
                                 bool parenthesized) {
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    return NULL;
  }
  node_t *param = create_func_param_node(allocator);
  func_param_node_set_name(param, name);
  node_set_location_span(
      param, node_get_location(name), node_get_location(name));

  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    parser_next(parser); /* ':' */
    parser_skip_trivia(parser);
    node_t *type = parser_read_expr_postfix(allocator, parser);
    if (!type) {
      location_t loc =
          expression_error_location(parser, node_get_location(name));
      parser_push_error(parser,
                        create_error(allocator, &loc, "expected capture type"));
      return param;
    }
    func_param_node_set_type(param, type);
    node_set_location_span(
        param, node_get_location(name), node_get_location(type));
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  if (token_is(token, CLUX_TOKEN_SYMBOL, "=")) {
    parser_next(parser); /* '=' */
    parser_skip_trivia(parser);
    node_t *value = parenthesized ? parser_read_expr(allocator, parser)
                                  : parser_read_expr_xor(allocator, parser);
    if (!value) {
      location_t loc =
          expression_error_location(parser, node_get_location(param));
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected capture default value"));
      return param;
    }
    func_param_node_set_default(param, value);
    node_set_location_span(
        param, node_get_location(name), node_get_location(value));
  }
  return param;
}

/* defer 捕获列表 |name, ...| 或 |(name = expr, ...)|（白皮书 §4.12）。
   两种写法都产出 CLUX_NODE_FUNC_CAPTURES，children 为一个
   CLUX_NODE_FUNC_PARAMS；空列表 `||` 不带 FUNC_PARAMS 子节点。
   调用前当前 token 必须是 '|'。 */
static node_t *read_defer_captures(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '|' */
  parser_skip_trivia(parser);

  node_t *node = create_func_captures_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);

  /* 空捕获列表 `||`。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    location_t close_loc = *token_get_location(token);
    parser_next(parser);
    node_set_location_span(node, &open_loc, &close_loc);
    return node;
  }

  bool parenthesized = token_is(token, CLUX_TOKEN_SYMBOL, "(");
  if (parenthesized) {
    parser_next(parser); /* '(' */
    parser_skip_trivia(parser);
  }

  node_t *params = create_func_params_node(allocator);
  node_set_location_span(params, &open_loc, &open_loc);

  for (;;) {
    token = parser_peek_token(parser);
    if (parenthesized && token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
      break;
    }
    if (!parenthesized && token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
      break;
    }
    node_t *item = read_capture_item(allocator, parser, parenthesized);
    if (!item) {
      location_t loc = expression_error_location(parser, &open_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected capture name in defer"));
      /* 容错：跳到下一个 ',' 或捕获列表闭合符。 */
      for (;;) {
        token = parser_peek_token(parser);
        if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ",") ||
            token_is(token, CLUX_TOKEN_SYMBOL, ")") ||
            token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
          break;
        }
        parser_next(parser);
      }
    } else {
      func_params_node_add_param(params, item);
    }
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      parser_next(parser); /* ',' */
      parser_skip_trivia(parser);
      continue;
    }
    break;
  }

  /* 形参列表区间 = 首个捕获项 .. 最后一个捕获项。 */
  size_t count = func_params_node_get_param_count(params);
  if (count) {
    node_set_location_span(
        params,
        node_get_location(func_params_node_get_param(params, 0)),
        node_get_location(func_params_node_get_param(params, count - 1)));
  }
  const location_t *end_loc = node_get_location(params);

  if (parenthesized) {
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
      end_loc = token_get_location(token);
      parser_next(parser);
    } else {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(parser,
                        create_error(allocator,
                                     &loc,
                                     "expected ')' to close defer captures"));
    }
  }
  func_captures_node_add_capture(node, params);

  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '|' to close defer captures"));
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* defer 语句 defer |captures|? { body }（白皮书 §4.12）。
   调用前当前 token 不一定是 defer，不是时返回 NULL 且游标不动。
   捕获列表可选；块必须存在。出错时按容错约定返回不完整节点并 push_error。 */
node_t *parser_read_stmt_defer(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "defer")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* defer */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_defer_node(allocator);
  node_set_location(node, &begin_loc);

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    node_t *captures = read_defer_captures(allocator, parser);
    stmt_defer_node_set_captures(node, captures);
    node_set_location_span(node, &begin_loc, node_get_location(captures));
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, node_get_location(node));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start defer body"));
    return node;
  }
  node_t *body = parser_read_stmt_block(allocator, parser);
  stmt_defer_node_set_body(node, body);
  node_set_location_span(node, &begin_loc, node_get_location(body));
  return node;
}
