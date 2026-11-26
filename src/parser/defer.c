#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* defer 语句 defer |captures|? { body }（白皮书 §4.12）。
   调用前当前 token 不一定是 defer，不是时返回 NULL 且游标不动。
   捕获列表与函数声明共用同一条解析通道（defer 本质上是函数，
   语法见白皮书 §3.3：裸名 |x| / 逐捕获括号 |(x: i32 = 1)| 两形态）。
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
    node_t *captures = parser_read_func_decl_captures(allocator, parser);
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
