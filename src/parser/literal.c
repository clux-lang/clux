#include "parser/literal.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"

/* 匹配当前 token 为指定 kind，成功则消费并返回新节点，否则返回 NULL。 */
static node_t *read_simple(allocator_t *allocator,
                           parser_t *parser,
                           token_kind_t kind,
                           node_kind_t node_kind) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != kind) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);
  node_t *node = create_node(allocator, node_kind);
  node_set_location(node, &loc);
  return node;
}

/* 匹配当前 token 为指定 kind 且文本等于 src。 */
static node_t *read_simple_text(allocator_t *allocator,
                                parser_t *parser,
                                token_kind_t kind,
                                const char *src,
                                node_kind_t node_kind) {
  token_t *token = parser_peek_token(parser);
  if (!token || !token_is(token, kind, src)) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);
  node_t *node = create_node(allocator, node_kind);
  node_set_location(node, &loc);
  return node;
}

node_t *parser_read_literal_nil(allocator_t *allocator, parser_t *parser) {
  return read_simple_text(
      allocator, parser, CLUX_TOKEN_KEYWORD, "nil", CLUX_NODE_LITERAL_NIL);
}

node_t *parser_read_literal_undefined(allocator_t *allocator,
                                      parser_t *parser) {
  return read_simple_text(allocator,
                          parser,
                          CLUX_TOKEN_KEYWORD,
                          "undefined",
                          CLUX_NODE_LITERAL_UNDEFINED);
}

node_t *parser_read_literal_string(allocator_t *allocator, parser_t *parser) {
  return read_simple(
      allocator, parser, CLUX_TOKEN_STRING, CLUX_NODE_LITERAL_STRING);
}

node_t *parser_read_literal_character(allocator_t *allocator,
                                      parser_t *parser) {
  return read_simple(
      allocator, parser, CLUX_TOKEN_CHARACTER, CLUX_NODE_LITERAL_CHARACTER);
}

node_t *parser_read_literal_boolean(allocator_t *allocator, parser_t *parser) {
  node_t *node = read_simple_text(
      allocator, parser, CLUX_TOKEN_KEYWORD, "true", CLUX_NODE_LITERAL_BOOLEAN);
  if (node) return node;
  return read_simple_text(allocator,
                          parser,
                          CLUX_TOKEN_KEYWORD,
                          "false",
                          CLUX_NODE_LITERAL_BOOLEAN);
}

node_t *parser_read_literal_identifier(allocator_t *allocator,
                                       parser_t *parser) {
  return read_simple(
      allocator, parser, CLUX_TOKEN_IDENTIFIER, CLUX_NODE_LITERAL_IDENTIFIER);
}

/* 内建类型名（白皮书 §2.1）。词法层把它们识别为关键字。 */
static const char *type_names[] = {
    "bool",
    "str",
    "void",
    "opaque",
    "i8",
    "i16",
    "i32",
    "i64",
    "u8",
    "u16",
    "u32",
    "u64",
    "f32",
    "f64",
    "isize",
    "usize",
    NULL,
};

node_t *parser_read_literal_type(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_KEYWORD) {
    return NULL;
  }
  for (size_t idx = 0; type_names[idx] != NULL; idx++) {
    if (token_is(token, CLUX_TOKEN_KEYWORD, type_names[idx])) {
      location_t loc = *token_get_location(token);
      parser_next(parser);
      node_t *node = create_node(allocator, CLUX_NODE_LITERAL_TYPE);
      node_set_location(node, &loc);
      return node;
    }
  }
  return NULL;
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

node_t *parser_read_literal_numeric(allocator_t *allocator, parser_t *parser) {
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
  node_t *node = create_node(allocator, CLUX_NODE_LITERAL_NUMERIC);
  node_set_location(node, &loc);
  return node;
}

node_t *parser_read_literal(allocator_t *allocator, parser_t *parser) {
  node_t *node = NULL;
  node = parser_read_literal_nil(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_literal_undefined(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_literal_boolean(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_literal_type(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_literal_character(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_literal_string(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_literal_identifier(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_literal_numeric(allocator, parser);
  if (node) {
    return node;
  }
  return NULL;
}