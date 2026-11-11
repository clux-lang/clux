#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_source_text.h"
#include "parser/node_stmt_assert.h"
#include "parser/node_stmt_static_assert.h"
#include "parser/parser.h"

/* 返回 pos 之后第一个非 trivia token；越界返回 NULL。 */
static token_t *peek_next_non_trivia(parser_t *parser, size_t pos) {
  vec_t *tokens = parser_get_tokens(parser);
  size_t count = vec_get_size(tokens);
  while (pos < count) {
    token_t *token = vec_get(tokens, pos);
    token_kind_t kind = token_get_kind(token);
    if (kind != CLUX_TOKEN_WHITESPACE && kind != CLUX_TOKEN_COMMENT &&
        kind != CLUX_TOKEN_MULTILINE_COMMENT) {
      return token;
    }
    pos++;
  }
  return NULL;
}

/* 用表达式自身的区间构造源码原文叶子：不复制文本，location 即原文区间。 */
static node_t *make_source_text(allocator_t *allocator, node_t *expr) {
  node_t *source = create_source_text_node(allocator);
  node_set_location(source, node_get_location(expr));
  return source;
}

/* 编译期断言 static_assert(cond, "msg");（白皮书 §4.16）。
   调用前当前 token 必须是 static_assert。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_static_assert(allocator_t *allocator,
                                       parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "static_assert")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* static_assert */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_static_assert_node(allocator);
  node_set_location(node, &begin_loc);
  const location_t *end_loc = &begin_loc;

  /* 头部 '('。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '(' after 'static_assert'"));
    return node;
  }
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  /* 条件表达式。 */
  node_t *condition = parser_read_expr(allocator, parser);
  if (!condition) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected condition expression"));
    return node;
  }
  stmt_static_assert_node_set_condition(node, condition);
  stmt_static_assert_node_set_source(node,
                                     make_source_text(allocator, condition));
  end_loc = node_get_location(condition);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 逗号。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected ',' after static_assert condition"));
    return node;
  }
  parser_next(parser); /* ',' */
  parser_skip_trivia(parser);

  /* 消息字符串。 */
  node_t *message = parser_read_literal_string(allocator, parser);
  if (!message) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected message string after ','"));
    return node;
  }
  stmt_static_assert_node_set_message(node, message);
  end_loc = node_get_location(message);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 闭合 ')'。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' to close static_assert"));
    return node;
  }
  end_loc = token_get_location(token);
  parser_next(parser);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 结尾 ';'。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected ';' after static_assert statement"));
  }
  node_set_location_span(node, &begin_loc, end_loc);
  return node;
}

/* 测试断言 assert(expr);（白皮书 §11.3）。
   assert 不是关键字：只有标识符 assert 后（跳过 trivia）紧跟 '(' 时才由本函数
   认领；否则返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_assert(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_IDENTIFIER, "assert")) {
    return NULL;
  }
  token_t *open = peek_next_non_trivia(parser, parser_get_position(parser) + 1);
  if (!token_is(open, CLUX_TOKEN_SYMBOL, "(")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* assert */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_assert_node(allocator);
  node_set_location(node, &begin_loc);
  const location_t *end_loc = &begin_loc;

  /* '(' 已由前瞻确认。 */
  end_loc = token_get_location(parser_peek_token(parser));
  parser_next(parser); /* '(' */
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 断言表达式。 */
  node_t *condition = parser_read_expr(allocator, parser);
  if (!condition) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected condition expression"));
    return node;
  }
  stmt_assert_node_set_condition(node, condition);
  stmt_assert_node_set_source(node, make_source_text(allocator, condition));
  end_loc = node_get_location(condition);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 闭合 ')'。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected ')' to close assert"));
    return node;
  }
  end_loc = token_get_location(token);
  parser_next(parser);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 结尾 ';'。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ';' after assert statement"));
  }
  node_set_location_span(node, &begin_loc, end_loc);
  return node;
}
