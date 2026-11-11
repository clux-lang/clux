#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_bitfield_field.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"

/* 单个位域字段 name: width（白皮书 §10）。
   调用前当前 token 不一定是标识符，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_bitfield_field(allocator_t *allocator, parser_t *parser) {
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    return NULL;
  }
  location_t name_loc = *node_get_location(name);
  node_t *node = create_bitfield_field_node(allocator);
  bitfield_field_node_set_name(node, name);
  node_set_location_span(node, &name_loc, &name_loc);

  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected ':' after bitfield field name"));
    return node;
  }
  parser_next(parser); /* ':' */
  parser_skip_trivia(parser);

  node_t *width = parser_read_expr(allocator, parser);
  if (!width) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected bitfield field width"));
    return node;
  }
  bitfield_field_node_set_width(node, width);
  node_set_location_span(node, &name_loc, node_get_location(width));
  return node;
}

/* 字段列表 { name: width; ... }。当前 token 必须是 '{'。
   ';' 严格跟随字段：每个字段后必须写 ';'，不允许省略末尾分号，
   也不允许空字段 `;`。空列表在语法阶段合法，是否至少一个字段由语义阶段检查。 */
static node_t *read_bitfield_fields(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  node_t *node = create_bitfield_fields_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  for (;;) {
    token = parser_peek_token(parser);
    if (!token) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected '}' to close bitfield body"));
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    node_t *field = read_bitfield_field(allocator, parser);
    if (!field) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected field in bitfield body"));
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
      continue;
    }
    bitfield_fields_node_add_field(node, field);
    end_loc = node_get_location(field);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      parser_skip_trivia(parser);
      continue;
    }
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ';' after bitfield field"));
    /* 容错：跳到下一个 ';' 或 '}'。 */
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

/* 位域声明 bitfield Name: Underlying { field: width; ... }（白皮书 §10）。
   装饰器属于外层语句（stmt_bitfield），本函数只解析声明本身。
   与 func_decl / union / enum / struct 一样不是前导可判定的：首 token 是
   bitfield 时必然是声明，任何不匹配都是硬错误。调用前当前 token 必须是
   bitfield。出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_bitfield_decl(allocator_t *allocator,
                                       parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "bitfield")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  node_t *keyword = create_symbol_node(allocator);
  node_set_location(keyword, &keyword_loc);
  parser_next(parser); /* bitfield */
  parser_skip_trivia(parser);

  node_t *node = create_bitfield_decl_node(allocator);
  bitfield_decl_node_set_keyword(node, keyword);
  node_set_location_span(node, &keyword_loc, &keyword_loc);
  const location_t *end_loc = &keyword_loc;

  /* 位域名。 */
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected name after 'bitfield'"));
    return node;
  }
  bitfield_decl_node_set_name(node, name);
  end_loc = node_get_location(name);
  node_set_location_span(node, &keyword_loc, end_loc);
  parser_skip_trivia(parser);

  /* 底层类型以 ':' 引导，必须显式写出。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ':' after bitfield name"));
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
  bitfield_decl_node_set_underlying(node, underlying);
  end_loc = node_get_location(underlying);
  node_set_location_span(node, &keyword_loc, end_loc);
  parser_skip_trivia(parser);

  /* 字段列表。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start bitfield body"));
    return node;
  }
  node_t *body = read_bitfield_fields(allocator, parser);
  bitfield_decl_node_set_body(node, body);
  node_set_location_span(node, &keyword_loc, node_get_location(body));
  return node;
}
