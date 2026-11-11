#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/node_builtin.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"

/* 编译器内建 #name(arg, ...)。
   调用前当前 token 不一定是 CLUX_TOKEN_BUILTIN，不是时返回 NULL 且游标不动。
   NAME 符号的 location 去掉开头的 '#'，节点自身区间仍从 '#' 开始。
   实参语法与普通调用一致，可以是 rest 表达式；名字与参数个数由 sema 校验。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_builtin(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_BUILTIN) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);

  /* 名字符号去掉 '#'：'#' 是 ASCII，偏移与列各加一即可。 */
  location_t name_loc = begin_loc;
  name_loc.begin.offset += 1;
  name_loc.begin.column += 1;
  node_t *name = create_symbol_node(allocator);
  node_set_location(name, &name_loc);

  node_t *node = create_builtin_node(allocator);
  builtin_node_set_name(node, name);
  node_set_location_span(node, &begin_loc, &name_loc);
  parser_next(parser); /* #name */
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '(' after builtin name"));
    return node;
  }
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  const location_t *end_loc = &name_loc;
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(node, &begin_loc, end_loc);
    return node;
  }

  for (;;) {
    node_t *arg = parser_read_expr_rest(allocator, parser);
    if (!arg) {
      arg = parser_read_expr(allocator, parser);
    }
    if (!arg) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression in builtin call"));
      break;
    }
    builtin_node_add_arg(node, arg);
    end_loc = node_get_location(arg);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      break;
    }
    parser_next(parser); /* ',' */
    parser_skip_trivia(parser);
  }

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' to close builtin call"));
  }
  node_set_location_span(node, &begin_loc, end_loc);
  return node;
}
