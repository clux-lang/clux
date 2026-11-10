#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_enum_decl.h"
#include "parser/node_enum_variant.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"

/* 单个枚举成员 Variant = value（白皮书 §3.6）。
   调用前当前 token 不一定是标识符，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_variant(allocator_t *allocator, parser_t *parser) {
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    return NULL;
  }
  location_t name_loc = *node_get_location(name);
  node_t *node = create_enum_variant_node(allocator);
  enum_variant_node_set_name(node, name);
  node_set_location_span(node, &name_loc, &name_loc);

  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "=")) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '=' after enum variant name"));
    return node;
  }
  parser_next(parser); /* '=' */
  parser_skip_trivia(parser);
  node_t *value = parser_read_expr(allocator, parser);
  if (!value) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected enum variant value after '='"));
    return node;
  }
  enum_variant_node_set_value(node, value);
  node_set_location_span(node, &name_loc, node_get_location(value));
  return node;
}

/* 成员列表 { Variant = value, ... }。当前 token 必须是 '{'。
   成员之间以 ',' 分隔，允许末尾逗号；空列表合法。 */
static node_t *read_variants(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  node_t *node = create_enum_variants_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  for (;;) {
    token = parser_peek_token(parser);
    if (!token) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected '}' to close enum body"));
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    node_t *variant = read_variant(allocator, parser);
    if (!variant) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected enum variant in enum body"));
      /* 容错：跳到下一个 ',' 或 '}'，避免卡在同一 token 上。 */
      for (;;) {
        token = parser_peek_token(parser);
        if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ",") ||
            token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
          break;
        }
        parser_next(parser);
      }
      if (token && token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
        parser_next(parser);
        parser_skip_trivia(parser);
      }
      continue;
    }
    enum_variants_node_add_variant(node, variant);
    end_loc = node_get_location(variant);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      parser_skip_trivia(parser);
      continue;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      continue;
    }
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ',' or '}' after enum variant"));
    /* 容错：跳到下一个 ',' 或 '}'。 */
    for (;;) {
      token = parser_peek_token(parser);
      if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ",") ||
          token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
        break;
      }
      parser_next(parser);
    }
    if (token && token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      parser_next(parser);
      parser_skip_trivia(parser);
    }
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* 枚举声明 enum Name: Underlying { Variant = value, ... }（白皮书 §3.6）。
   装饰器属于外层语句（stmt_enum），本函数只解析声明本身。
   与 func_decl / union 一样不是前导可判定的：首 token 是 enum 时必然是
   声明，任何不匹配都是硬错误。调用前当前 token 必须是 enum。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_enum_decl(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "enum")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  node_t *keyword = create_symbol_node(allocator);
  node_set_location(keyword, &keyword_loc);
  parser_next(parser); /* enum */
  parser_skip_trivia(parser);

  node_t *node = create_enum_decl_node(allocator);
  enum_decl_node_set_keyword(node, keyword);
  node_set_location_span(node, &keyword_loc, &keyword_loc);
  const location_t *end_loc = &keyword_loc;

  /* 枚举名。 */
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected name after 'enum'"));
    return node;
  }
  enum_decl_node_set_name(node, name);
  end_loc = node_get_location(name);
  node_set_location_span(node, &keyword_loc, end_loc);
  parser_skip_trivia(parser);

  /* 底层类型以 ':' 引导，必须显式写出。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ':' after enum name"));
    return node;
  }
  parser_next(parser); /* ':' */
  parser_skip_trivia(parser);
  node_t *underlying = parser_read_expr(allocator, parser);
  if (!underlying) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected underlying type after ':'"));
    return node;
  }
  enum_decl_node_set_underlying(node, underlying);
  end_loc = node_get_location(underlying);
  node_set_location_span(node, &keyword_loc, end_loc);
  parser_skip_trivia(parser);

  /* 成员列表。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start enum body"));
    return node;
  }
  node_t *body = read_variants(allocator, parser);
  enum_decl_node_set_body(node, body);
  node_set_location_span(node, &keyword_loc, node_get_location(body));
  return node;
}
