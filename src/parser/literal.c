#include "parser/literal.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"

/* 匹配当前 token 为指定 kind，成功则消费并返回新节点，否则返回 NULL。 */
static node_t *read_simple(allocator_t *allocator,
                           parser_t *parser,
                           node_t *parent,
                           token_kind_t kind,
                           node_kind_t node_kind) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != kind) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);
  return create_node(allocator, node_kind, parent, loc);
}

/* 匹配当前 token 为指定 kind 且文本等于 src。 */
static node_t *read_simple_text(allocator_t *allocator,
                                parser_t *parser,
                                node_t *parent,
                                token_kind_t kind,
                                const char *src,
                                node_kind_t node_kind) {
  token_t *token = parser_peek_token(parser);
  if (!token || !token_is(token, kind, src)) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);
  return create_node(allocator, node_kind, parent, loc);
}

node_t *parser_read_literal_nil(allocator_t *allocator,
                                parser_t *parser,
                                node_t *parent) {
  return read_simple_text(allocator,
                          parser,
                          parent,
                          CLUX_TOKEN_KEYWORD,
                          "nil",
                          CLUX_NODE_LITERAL_NIL);
}

node_t *parser_read_literal_undefined(allocator_t *allocator,
                                      parser_t *parser,
                                      node_t *parent) {
  return read_simple_text(allocator,
                          parser,
                          parent,
                          CLUX_TOKEN_KEYWORD,
                          "undefined",
                          CLUX_NODE_LITERAL_UNDEFINED);
}

node_t *parser_read_literal_string(allocator_t *allocator,
                                   parser_t *parser,
                                   node_t *parent) {
  return read_simple(
      allocator, parser, parent, CLUX_TOKEN_STRING, CLUX_NODE_LITERAL_STRING);
}

node_t *parser_read_literal_character(allocator_t *allocator,
                                      parser_t *parser,
                                      node_t *parent) {
  return read_simple(allocator,
                     parser,
                     parent,
                     CLUX_TOKEN_CHARACTER,
                     CLUX_NODE_LITERAL_CHARACTER);
}

node_t *parser_read_literal_boolean(allocator_t *allocator,
                                    parser_t *parser,
                                    node_t *parent) {
  node_t *node = read_simple_text(allocator,
                                  parser,
                                  parent,
                                  CLUX_TOKEN_KEYWORD,
                                  "true",
                                  CLUX_NODE_LITERAL_BOOLEAN);
  if (node) return node;
  return read_simple_text(allocator,
                          parser,
                          parent,
                          CLUX_TOKEN_KEYWORD,
                          "false",
                          CLUX_NODE_LITERAL_BOOLEAN);
}

node_t *parser_read_literal_identifier(allocator_t *allocator,
                                       parser_t *parser,
                                       node_t *parent) {
  return read_simple(allocator,
                     parser,
                     parent,
                     CLUX_TOKEN_IDENTIFIER,
                     CLUX_NODE_LITERAL_IDENTIFIER);
}

/* 数字类型后缀（词法层识别为关键字）。 */
static const char *numeric_suffixes[] = {
    "i8",
    "i16",
    "i32",
    "i64",
    "isize",
    "u8",
    "u16",
    "u32",
    "u64",
    "usize",
    "f32",
    "f64",
    NULL,
};

static bool is_numeric_suffix(token_t *token) {
  if (token_get_kind(token) != CLUX_TOKEN_KEYWORD) return false;
  for (size_t idx = 0; numeric_suffixes[idx] != NULL; idx++) {
    if (token_is(token, CLUX_TOKEN_KEYWORD, numeric_suffixes[idx])) {
      return true;
    }
  }
  return false;
}

node_t *parser_read_literal_numeric(allocator_t *allocator,
                                    parser_t *parser,
                                    node_t *parent) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_NUMERIC) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);

  /* 后缀必须紧邻数字（无空白/注释），即下一个 token 的起点等于数字的终点。 */
  token_t *suffix = parser_peek_token(parser);
  if (suffix && is_numeric_suffix(suffix) &&
      token_get_location(suffix)->begin.offset == loc.end.offset) {
    loc.end = token_get_location(suffix)->end;
    parser_next(parser);
  }
  return create_node(allocator, CLUX_NODE_LITERAL_NUMERIC, parent, loc);
}
