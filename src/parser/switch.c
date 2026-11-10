#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_switch.h"
#include "parser/node_switch_branch.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* 模式列表 (pattern, ...)（白皮书 §4.5）。当前 token 必须是 '('。
   模式在语法阶段按表达式解析；空列表报错。 */
static node_t *read_patterns(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  node_t *node = create_switch_patterns_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc = expression_error_location(parser, &open_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected pattern in switch branch"));
  } else {
    for (;;) {
      token = parser_peek_token(parser);
      if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
        break;
      }
      node_t *pattern = parser_read_expr(allocator, parser);
      if (!pattern) {
        location_t loc = expression_error_location(parser, end_loc);
        parser_push_error(
            parser,
            create_error(allocator, &loc, "expected pattern in switch branch"));
        /* 容错：跳到下一个 ',' 或 ')'。 */
        for (;;) {
          token = parser_peek_token(parser);
          if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ",") ||
              token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
            break;
          }
          parser_next(parser);
        }
      } else {
        switch_patterns_node_add_pattern(node, pattern);
        end_loc = node_get_location(pattern);
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
  }

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' after switch patterns"));
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* 单个 switch 分支：(pattern, ...) -> { body } 或 default -> { body }。
   调用前当前 token 不一定是分支起始，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_branch(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t begin_loc = *token_get_location(token);
  node_t *node = create_switch_branch_node(allocator);
  node_set_location(node, &begin_loc);

  if (token_is(token, CLUX_TOKEN_KEYWORD, "default")) {
    parser_next(parser); /* default */
    parser_skip_trivia(parser);
  } else if (token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    node_t *patterns = read_patterns(allocator, parser);
    switch_branch_node_set_patterns(node, patterns);
    node_set_location_span(node, &begin_loc, node_get_location(patterns));
    parser_skip_trivia(parser);
  } else {
    delete_node(&node);
    return NULL;
  }

  /* '->' 引导分支体。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "->")) {
    location_t loc = expression_error_location(parser, node_get_location(node));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '->' after switch branch"));
    return node;
  }
  parser_next(parser); /* '->' */
  parser_skip_trivia(parser);

  /* 分支体必须是块。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, node_get_location(node));
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected '{' to start switch branch body"));
    return node;
  }
  node_t *body = parser_read_stmt_block(allocator, parser);
  switch_branch_node_set_body(node, body);
  node_set_location_span(node, &begin_loc, node_get_location(body));
  return node;
}

/* 分支列表 { branch... }。当前 token 必须是 '{'。 */
static node_t *read_branches(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  node_t *node = create_switch_branches_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  for (;;) {
    token = parser_peek_token(parser);
    if (!token) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected '}' to close switch body"));
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    node_t *branch = read_branch(allocator, parser);
    if (!branch) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser, create_error(allocator, &loc, "expected switch branch"));
      parser_next(parser);
      continue;
    }
    switch_branches_node_add_branch(node, branch);
    end_loc = node_get_location(branch);
    parser_skip_trivia(parser);
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* switch 语句 switch (value) { branch... }（白皮书 §4.5）。
   匹配值必须用圆括号包围；分支写作 (pattern, ...) -> { ... } 或
   default -> { ... }，分支之间不需要分隔符。
   调用前当前 token 不一定是 switch，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_switch(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "switch")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* switch */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_switch_node(allocator);
  node_set_location(node, &begin_loc);

  /* 匹配值必须带圆括号。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected '(' after 'switch'"));
    return node;
  }
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  node_t *value = parser_read_expr(allocator, parser);
  if (!value) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected switch value expression"));
    return node;
  }
  stmt_switch_node_set_value(node, value);
  node_set_location_span(node, &begin_loc, node_get_location(value));
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc =
        expression_error_location(parser, node_get_location(value));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' after switch value"));
    return node;
  }
  parser_next(parser); /* ')' */
  parser_skip_trivia(parser);

  /* 分支列表。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc =
        expression_error_location(parser, node_get_location(value));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start switch body"));
    return node;
  }
  node_t *body = read_branches(allocator, parser);
  stmt_switch_node_set_body(node, body);
  node_set_location_span(node, &begin_loc, node_get_location(body));
  return node;
}
