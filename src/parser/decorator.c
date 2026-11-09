#include "parser/decorator.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"

/* 错误位置：优先指向当前未消费的 token；已到流末尾时退化为 fallback 的终点。 */
static location_t decorator_error_location(parser_t *parser,
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

/* 装饰器 [[ <expr> ]]（白皮书 §3.2、§9.1）。
   `[[` 与 `]]` 都不在词法表中（与嵌套下标冲突），这里按相邻字符位置组合。
   两个定界符都是整体：起始两个 '[' 与结束两个 ']' 都必须相邻，
   `[ [trace]]`、`[[trace] ]` 都不构成装饰器。
   调用前当前 token 不一定是 '['，不是 `[[` 时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_decorator(allocator_t *allocator, parser_t *parser) {
  token_t *first = parser_peek_token(parser);
  if (!token_is(first, CLUX_TOKEN_SYMBOL, "[")) {
    return NULL;
  }
  token_t *second = peek_token_at(parser, 1);
  if (!token_is(second, CLUX_TOKEN_SYMBOL, "[")) {
    return NULL;
  }
  location_t open_loc = *token_get_location(first);
  open_loc.end = token_get_location(second)->end;
  parser_next(parser); /* 第一个 '[' */
  parser_next(parser); /* 第二个 '[' */
  parser_skip_trivia(parser);

  node_t *node = create_node(allocator, CLUX_NODE_DECORATOR);
  node_resize_children(node, CLUX_DECORATOR_CHILDREN_SIZE);
  node_set_location(node, &open_loc);
  const location_t *end_loc = &open_loc;

  node_t *expr = parser_read_expr(allocator, parser);
  if (!expr) {
    location_t loc = decorator_error_location(parser, &open_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression in decorator"));
    return node;
  }
  node_set_child(node, CLUX_DECORATOR_EXPR, expr);
  end_loc = node_get_location(expr);

  parser_skip_trivia(parser);
  /* `]]` 是整体：两个 ']' 必须相邻，中间有 trivia 不构成装饰器闭合。 */
  token_t *close_first = parser_peek_token(parser);
  token_t *close_second = peek_token_at(parser, 1);
  if (!token_is(close_first, CLUX_TOKEN_SYMBOL, "]") ||
      !token_is(close_second, CLUX_TOKEN_SYMBOL, "]")) {
    location_t loc = decorator_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ']]' to close decorator"));
    return node;
  }
  location_t close_loc = *token_get_location(close_second);
  parser_next(parser); /* 第一个 ']' */
  parser_next(parser); /* 第二个 ']' */

  location_t loc = open_loc;
  loc.end = close_loc.end;
  node_set_location(node, &loc);
  return node;
}
