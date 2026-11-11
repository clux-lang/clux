#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_test.h"
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

/* 测试块 test "Suite.Name" { body }（白皮书 §3.14、§11.1）。
   test 不是关键字：只有 `test` 标识符后接字符串字面量时才按测试块解析；
   否则返回 NULL 且游标不动，`test` 仍可作普通标识符。
   是否只允许出现在顶层由语义阶段检查。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_test(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_IDENTIFIER, "test")) {
    return NULL;
  }
  token_t *name_token =
      peek_next_non_trivia(parser, parser_get_position(parser) + 1);
  if (!name_token || token_get_kind(name_token) != CLUX_TOKEN_STRING) {
    return NULL;
  }

  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* test */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_test_node(allocator);
  node_set_location(node, &begin_loc);

  node_t *name = parser_read_literal_string(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected test name string after 'test'"));
    return node;
  }
  stmt_test_node_set_name(node, name);
  node_set_location_span(node, &begin_loc, node_get_location(name));
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, node_get_location(name));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start test body"));
    return node;
  }
  node_t *body = parser_read_stmt_block(allocator, parser);
  stmt_test_node_set_body(node, body);
  node_set_location_span(node, &begin_loc, node_get_location(body));
  return node;
}
