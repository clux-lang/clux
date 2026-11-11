#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_stmt_reexport.h"
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

/* 重新导出语句 export * from "<path>";（白皮书 §8.4）。
   只有 `export` 后（跳过 trivia）紧跟 '*' 时才由本函数认领；否则返回 NULL
   且游标不动，`export <decl>` 仍由各声明语句处理。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_reexport(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "export")) {
    return NULL;
  }
  token_t *star = peek_next_non_trivia(parser, parser_get_position(parser) + 1);
  if (!token_is(star, CLUX_TOKEN_SYMBOL, "*")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* export */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_reexport_node(allocator);
  node_set_location(node, &begin_loc);
  const location_t *end_loc = &begin_loc;

  /* '*' 已由前瞻确认。 */
  token = parser_peek_token(parser);
  end_loc = token_get_location(token);
  parser_next(parser); /* '*' */
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* from 关键字。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "from")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected 'from' after 'export *'"));
    return node;
  }
  parser_next(parser); /* from */
  parser_skip_trivia(parser);

  /* 模块路径字符串。 */
  node_t *path = parser_read_literal_string(allocator, parser);
  if (!path) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected module path string after 'from'"));
    return node;
  }
  stmt_reexport_node_set_path(node, path);
  end_loc = node_get_location(path);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 结尾 ';'。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected ';' after re-export statement"));
  }
  node_set_location_span(node, &begin_loc, end_loc);
  return node;
}
