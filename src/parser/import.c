#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_stmt_import.h"
#include "parser/parser.h"

/* 导入语句 import <alias> from "<path>";（白皮书 §8.2）。
   调用前当前 token 必须是 import。别名是标识符，路径是字符串字面量。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_import(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "import")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* import */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_import_node(allocator);
  node_set_location(node, &begin_loc);
  const location_t *end_loc = &begin_loc;

  /* 模块别名。 */
  node_t *alias = parser_read_literal_identifier(allocator, parser);
  if (!alias) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected module alias after 'import'"));
    return node;
  }
  stmt_import_node_set_alias(node, alias);
  end_loc = node_get_location(alias);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* from 关键字。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "from")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected 'from' after module alias"));
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
  stmt_import_node_set_path(node, path);
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
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ';' after import statement"));
  }
  node_set_location_span(node, &begin_loc, end_loc);
  return node;
}
