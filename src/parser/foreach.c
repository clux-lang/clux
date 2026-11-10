#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"
#include "parser/statement.h"

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

/* 解析 foreach 语句的公共部分。调用前当前 token 必须是 foreach。
   comptime 决定节点种类；begin_loc 是语句起点（普通 foreach 为 foreach
   关键字，comptime foreach 为 comptime）。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_foreach_stmt(allocator_t *allocator,
                                 parser_t *parser,
                                 bool comptime,
                                 location_t begin_loc) {
  parser_next(parser); /* foreach */
  parser_skip_trivia(parser);

  node_t *node = comptime ? create_stmt_comptime_foreach_node(allocator)
                          : create_stmt_foreach_node(allocator);
  node_set_location(node, &begin_loc);

  /* 头部必须带圆括号。 */
  token_t *token = parser_peek_token(parser);
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

/* foreach 语句 foreach ([var] name [: type]? of iterable) { body }
   （白皮书 §4.9）。var 可省略（只读迭代）；类型标注只取后缀层，
   `of` 是关键字，天然终止类型解析。
   调用前当前 token 不一定是 foreach，不是时返回 NULL 且游标不动。 */
node_t *parser_read_stmt_foreach(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "foreach")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  return read_foreach_stmt(allocator, parser, false, begin_loc);
}

/* comptime foreach 语句 comptime foreach (...) { ... }（白皮书 §4.15）。
   在编译期展开。调用前当前 token 不一定是 comptime，不是或后接的不是
   foreach 时返回 NULL 且游标不动（`comptime if` 留给 comptime if 语句）。 */
node_t *parser_read_stmt_comptime_foreach(allocator_t *allocator,
                                          parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "comptime")) {
    return NULL;
  }
  token_t *after =
      peek_next_non_trivia(parser, parser_get_position(parser) + 1);
  if (!token_is(after, CLUX_TOKEN_KEYWORD, "foreach")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* comptime */
  parser_skip_trivia(parser);
  return read_foreach_stmt(allocator, parser, true, begin_loc);
}
