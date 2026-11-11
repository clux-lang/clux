#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_interface_decl.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"

/* 接口成员列表 { func signature; ... }。当前 token 必须是 '{'。
   成员只允许无函数体的 func/cfunc 签名；空列表在语法阶段合法。 */
static node_t *read_interface_members(allocator_t *allocator,
                                      parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  node_t *node = create_interface_members_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  for (;;) {
    token = parser_peek_token(parser);
    if (!token) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(parser,
                        create_error(allocator,
                                     &loc,
                                     "expected '}' to close interface body"));
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    if (token_is(token, CLUX_TOKEN_KEYWORD, "func") ||
        token_is(token, CLUX_TOKEN_KEYWORD, "cfunc")) {
      node_t *signature = parser_read_expr_func_signature(allocator, parser);
      if (signature) {
        interface_members_node_add_member(node, signature);
        end_loc = node_get_location(signature);
        parser_skip_trivia(parser);
        continue;
      }
    }
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected function signature in interface body"));
    /* 容错：跳到下一个 ';' 或 '}'，避免卡在同一 token 上。 */
    for (;;) {
      token = parser_peek_token(parser);
      if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ";") ||
          token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
        break;
      }
      parser_next(parser);
    }
    if (token && token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
      parser_next(parser);
      parser_skip_trivia(parser);
    }
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* 接口声明 interface Name[generics]? { func signature; ... }
   （白皮书 §3.10、§6.9）。装饰器与 export 属于外层语句（stmt_interface），
   本函数只解析声明本身。调用前当前 token 必须是 interface。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_interface_decl(allocator_t *allocator,
                                        parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "interface")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  node_t *keyword = create_symbol_node(allocator);
  node_set_location(keyword, &keyword_loc);
  parser_next(parser); /* interface */
  parser_skip_trivia(parser);

  node_t *node = create_interface_decl_node(allocator);
  interface_decl_node_set_keyword(node, keyword);
  node_set_location_span(node, &keyword_loc, &keyword_loc);
  const location_t *end_loc = &keyword_loc;

  /* 接口名。 */
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected name after 'interface'"));
    return node;
  }
  interface_decl_node_set_name(node, name);
  end_loc = node_get_location(name);
  node_set_location_span(node, &keyword_loc, end_loc);
  parser_skip_trivia(parser);

  /* 可选泛型参数列表。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
    node_t *generics = parser_read_func_decl_generics(allocator, parser);
    interface_decl_node_set_generics(node, generics);
    end_loc = node_get_location(generics);
    node_set_location_span(node, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
  }

  /* 成员列表。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start interface body"));
    return node;
  }
  node_t *body = read_interface_members(allocator, parser);
  interface_decl_node_set_body(node, body);
  node_set_location_span(node, &keyword_loc, node_get_location(body));
  return node;
}
