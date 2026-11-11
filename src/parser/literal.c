#include "parser/literal.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/node_literal.h"
#include "parser/parser.h"
#include <unicode/utf8.h>

/* 匹配当前 token 为指定 kind，成功则消费并返回新节点，否则返回 NULL。 */
static node_t *read_simple(allocator_t *allocator,
                           parser_t *parser,
                           token_kind_t kind,
                           node_t *(*create)(allocator_t *)) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != kind) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);
  node_t *node = create(allocator);
  node_set_location(node, &loc);
  return node;
}

/* 匹配当前 token 为指定 kind 且文本等于 src。 */
static node_t *read_simple_text(allocator_t *allocator,
                                parser_t *parser,
                                token_kind_t kind,
                                const char *src,
                                node_t *(*create)(allocator_t *)) {
  token_t *token = parser_peek_token(parser);
  if (!token || !token_is(token, kind, src)) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);
  node_t *node = create(allocator);
  node_set_location(node, &loc);
  return node;
}

node_t *parser_read_literal_nil(allocator_t *allocator, parser_t *parser) {
  return read_simple_text(
      allocator, parser, CLUX_TOKEN_KEYWORD, "nil", create_literal_nil_node);
}

node_t *parser_read_literal_undefined(allocator_t *allocator,
                                      parser_t *parser) {
  return read_simple_text(allocator,
                          parser,
                          CLUX_TOKEN_KEYWORD,
                          "undefined",
                          create_literal_undefined_node);
}

static bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

static bool is_octal_digit(char c) { return c >= '0' && c <= '7'; }

static bool is_binary_digit(char c) { return c == '0' || c == '1'; }

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

/* 校验一个转义序列。s 指向 '\\'，end 是字面量内容末尾（不含闭合引号）。
   成功返回消耗的字节数并写 value；失败 push_error 并返回 0。 */
static size_t scan_escape(allocator_t *allocator,
                          parser_t *parser,
                          const location_t *loc,
                          const char *s,
                          const char *end,
                          uint32_t *value) {
  if (s + 1 >= end) {
    parser_push_error(
        parser, create_error(allocator, loc, "incomplete escape sequence"));
    return 0;
  }
  char e = s[1];
  switch (e) {
  case 'n':
    *value = 0x0A;
    return 2;
  case 't':
    *value = 0x09;
    return 2;
  case 'r':
    *value = 0x0D;
    return 2;
  case '\\':
    *value = '\\';
    return 2;
  case '\'':
    *value = '\'';
    return 2;
  case '"':
    *value = '"';
    return 2;
  case '0':
    *value = 0;
    return 2;
  case 'x': {
    size_t digits = 0;
    uint32_t v = 0;
    while (digits < 2 && s + 2 + digits < end) {
      char h = s[2 + digits];
      if (!is_hex_digit(h)) break;
      v = v * 16 + (uint32_t)hex_value(h);
      digits++;
    }
    if (digits == 0) {
      parser_push_error(
          parser,
          create_error(allocator, loc, "expected hex digits after '\\x'"));
      return 0;
    }
    *value = v;
    return 2 + digits;
  }
  default:
    parser_push_error(
        parser,
        create_error(allocator, loc, "invalid escape sequence '\\%c'", e));
    return 0;
  }
}

/* 字符串字面量内容校验（白皮书 §1.8）：合法转义、不能跨行。 */
static void validate_string_literal(allocator_t *allocator,
                                    parser_t *parser,
                                    const location_t *loc) {
  const char *s = loc->begin.offset + 1;
  const char *end = loc->end.offset - 1;
  while (s < end) {
    if (*s == '\n' || *s == '\r') {
      parser_push_error(
          parser,
          create_error(allocator, loc, "string literal cannot span lines"));
      return;
    }
    if (*s == '\\') {
      uint32_t value;
      size_t consumed = scan_escape(allocator, parser, loc, s, end, &value);
      if (consumed == 0) return;
      s += consumed;
      continue;
    }
    const uint8_t *u = (const uint8_t *)s;
    int32_t i = 0;
    UChar32 c;
    U8_NEXT(u, i, (int32_t)(end - s), c);
    if (c < 0 || i <= 0) {
      parser_push_error(
          parser,
          create_error(allocator, loc, "invalid UTF-8 in string literal"));
      return;
    }
    s += i;
  }
}

/* 字符字面量内容校验（白皮书 §1.9）：恰好一个 u8 范围内的字符。 */
static void validate_character_literal(allocator_t *allocator,
                                       parser_t *parser,
                                       const location_t *loc) {
  const char *s = loc->begin.offset + 1;
  const char *end = loc->end.offset - 1;
  if (s >= end) {
    parser_push_error(
        parser,
        create_error(allocator, loc, "character literal cannot be empty"));
    return;
  }
  if (*s == '\n' || *s == '\r') {
    parser_push_error(
        parser,
        create_error(allocator, loc, "character literal cannot span lines"));
    return;
  }

  uint32_t value = 0;
  size_t consumed = 0;
  if (*s == '\\') {
    consumed = scan_escape(allocator, parser, loc, s, end, &value);
    if (consumed == 0) return;
  } else {
    const uint8_t *u = (const uint8_t *)s;
    int32_t i = 0;
    UChar32 c;
    U8_NEXT(u, i, (int32_t)(end - s), c);
    if (c < 0 || i <= 0) {
      parser_push_error(
          parser,
          create_error(allocator, loc, "invalid UTF-8 in character literal"));
      return;
    }
    value = (uint32_t)c;
    consumed = (size_t)i;
  }

  if (value > 0xFF) {
    parser_push_error(parser,
                      create_error(allocator,
                                   loc,
                                   "character literal value out of u8 range"));
    return;
  }
  if (s + consumed != end) {
    parser_push_error(
        parser,
        create_error(
            allocator,
            loc,
            "character literal must contain exactly one u8 character"));
  }
}

node_t *parser_read_literal_string(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_STRING) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);
  validate_string_literal(allocator, parser, &loc);
  node_t *node = create_literal_string_node(allocator);
  node_set_location(node, &loc);
  return node;
}

node_t *parser_read_literal_character(allocator_t *allocator,
                                      parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_CHARACTER) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser);
  validate_character_literal(allocator, parser, &loc);
  node_t *node = create_literal_character_node(allocator);
  node_set_location(node, &loc);
  return node;
}

node_t *parser_read_literal_boolean(allocator_t *allocator, parser_t *parser) {
  node_t *node = read_simple_text(allocator,
                                  parser,
                                  CLUX_TOKEN_KEYWORD,
                                  "true",
                                  create_literal_boolean_node);
  if (node) return node;
  return read_simple_text(allocator,
                          parser,
                          CLUX_TOKEN_KEYWORD,
                          "false",
                          create_literal_boolean_node);
}

node_t *parser_read_literal_identifier(allocator_t *allocator,
                                       parser_t *parser) {
  return read_simple(
      allocator, parser, CLUX_TOKEN_IDENTIFIER, create_literal_identifier_node);
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
      node_t *node = create_literal_type_node(allocator);
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

/* 数字字面量内容校验（白皮书 §1.6、§1.7）：
   进制前缀后必须有数字；0o/0b 只允许对应进制数字；整数/浮点后缀必须匹配。 */
static void validate_numeric_literal(allocator_t *allocator,
                                     parser_t *parser,
                                     const location_t *loc,
                                     token_t *suffix) {
  const char *p = loc->begin.offset;
  const char *end = loc->end.offset;
  bool is_float = false;

  if (end - p >= 2 && p[0] == '0') {
    char kind = p[1];
    if (kind == 'x' || kind == 'X') {
      if (end - p == 2) {
        parser_push_error(
            parser, create_error(allocator, loc, "expected digits after '0x'"));
      }
    } else if (kind == 'o' || kind == 'O') {
      if (end - p == 2) {
        parser_push_error(
            parser, create_error(allocator, loc, "expected digits after '0o'"));
      } else {
        for (const char *q = p + 2; q < end; q++) {
          if (!is_octal_digit(*q)) {
            parser_push_error(
                parser,
                create_error(
                    allocator, loc, "invalid digit '%c' in octal literal", *q));
            return;
          }
        }
      }
    } else if (kind == 'b' || kind == 'B') {
      if (end - p == 2) {
        parser_push_error(
            parser, create_error(allocator, loc, "expected digits after '0b'"));
      } else {
        for (const char *q = p + 2; q < end; q++) {
          if (!is_binary_digit(*q)) {
            parser_push_error(
                parser,
                create_error(allocator,
                             loc,
                             "invalid digit '%c' in binary literal",
                             *q));
            return;
          }
        }
      }
    } else {
      for (const char *q = p; q < end; q++) {
        if (*q == '.' || *q == 'e' || *q == 'E') {
          is_float = true;
          break;
        }
      }
    }
  } else {
    for (const char *q = p; q < end; q++) {
      if (*q == '.' || *q == 'e' || *q == 'E') {
        is_float = true;
        break;
      }
    }
  }

  if (!suffix) {
    return;
  }
  bool float_suffix = token_is(suffix, CLUX_TOKEN_KEYWORD, "f32") ||
                      token_is(suffix, CLUX_TOKEN_KEYWORD, "f64");
  if (is_float && !float_suffix) {
    parser_push_error(
        parser,
        create_error(allocator,
                     token_get_location(suffix),
                     "floating literal requires 'f32' or 'f64' suffix"));
  } else if (!is_float && float_suffix) {
    parser_push_error(
        parser,
        create_error(allocator,
                     token_get_location(suffix),
                     "integer literal cannot use floating suffix"));
  }
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
  token_t *used_suffix = NULL;
  if (suffix && is_numeric_suffix(suffix) &&
      token_get_location(suffix)->begin.offset == loc.end.offset) {
    used_suffix = suffix;
    loc.end = token_get_location(suffix)->end;
    parser_next(parser);
  }
  validate_numeric_literal(
      allocator, parser, token_get_location(token), used_suffix);
  node_t *node = create_literal_numeric_node(allocator);
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