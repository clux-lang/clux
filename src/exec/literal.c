/* literal 求值：AST 节点只记 location 区间，文本由这里从源码解码。
   数字支持 0x/0o/0b/十进制与 i8..f64/isize/usize 后缀（后缀决定类型，
   无后缀整数默认 i32；浮点后缀由语法层强制要求）。字符串/字符走与
   parser 相同的转义集；内建类型名产 type value（语法层类型就是
   普通表达式，白皮书 §2.1）。 */
#include "exec/literal.h"
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "parser/node.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

clux_value_t *exec_literal_identifier(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  char *str = location_get(loc, allocator);
  clux_value_t *value = clux_context_lookup(ctx, str, loc);
  allocator_free(allocator, (void **)&str);
  return value;
}

static bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

/* 尾部剥类型后缀：返回后缀文本或 NULL；*num_len 写入数字部分长度。 */
static const char *strip_suffix(const char *text, size_t len, size_t *num_len) {
  static const char *suffixes[] = {"isize",
                                   "usize",
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
                                   NULL};
  for (size_t idx = 0; suffixes[idx] != NULL; idx++) {
    size_t slen = strlen(suffixes[idx]);
    if (len > slen && strncmp(text + len - slen, suffixes[idx], slen) == 0) {
      *num_len = len - slen;
      return suffixes[idx];
    }
  }
  *num_len = len;
  return NULL;
}

static clux_value_t *make_int(clux_context_t *ctx,
                              const char *suffix,
                              uint64_t magnitude,
                              bool negative,
                              const location_t *loc) {
  location_t *where = (location_t *)loc;
  int64_t signed_value = negative ? -(int64_t)magnitude : (int64_t)magnitude;
  if (!suffix) {
    return clux_context_create_i32(ctx, (int32_t)signed_value, NULL, where);
  }
  if (strcmp(suffix, "i8") == 0) {
    return clux_context_create_i8(ctx, (int8_t)signed_value, NULL, where);
  }
  if (strcmp(suffix, "i16") == 0) {
    return clux_context_create_i16(ctx, (int16_t)signed_value, NULL, where);
  }
  if (strcmp(suffix, "i32") == 0) {
    return clux_context_create_i32(ctx, (int32_t)signed_value, NULL, where);
  }
  if (strcmp(suffix, "i64") == 0) {
    return clux_context_create_i64(ctx, signed_value, NULL, where);
  }
  if (strcmp(suffix, "isize") == 0) {
    return clux_context_create_isize(ctx, (intptr_t)signed_value, NULL, where);
  }
  if (strcmp(suffix, "u8") == 0) {
    return clux_context_create_u8(ctx, (uint8_t)magnitude, NULL, where);
  }
  if (strcmp(suffix, "u16") == 0) {
    return clux_context_create_u16(ctx, (uint16_t)magnitude, NULL, where);
  }
  if (strcmp(suffix, "u32") == 0) {
    return clux_context_create_u32(ctx, (uint32_t)magnitude, NULL, where);
  }
  if (strcmp(suffix, "u64") == 0) {
    return clux_context_create_u64(ctx, magnitude, NULL, where);
  }
  if (strcmp(suffix, "usize") == 0) {
    return clux_context_create_usize(ctx, (size_t)magnitude, NULL, where);
  }
  /* suffix 只来自 strip_suffix 白名单，未知后缀是不可达的调用方违约 */
  fprintf(stderr, "invalid integer suffix '%s'", suffix);
  abort();
}

static clux_value_t *make_float(clux_context_t *ctx,
                                const char *suffix,
                                const char *digits,
                                size_t digits_len,
                                const location_t *loc) {
  location_t *where = (location_t *)loc;
  char buffer[64];
  if (digits_len >= sizeof(buffer)) {
    return clux_context_create_error_message(
        ctx, where, "numeric literal too long");
  }
  memcpy(buffer, digits, digits_len);
  buffer[digits_len] = '\0';
  if (strcmp(suffix, "f32") == 0) {
    return clux_context_create_f32(ctx, strtof(buffer, NULL), NULL, where);
  }
  if (strcmp(suffix, "f64") == 0) {
    return clux_context_create_f64(ctx, strtod(buffer, NULL), NULL, where);
  }
  /* 同上：f32/f64 已在调用方提前分流，未知后缀不可达 */
  fprintf(stderr, "invalid float suffix '%s'", suffix);
  abort();
}

static int digit_base(const char *digits, size_t len) {
  if (len > 2 && digits[0] == '0') {
    if (digits[1] == 'x' || digits[1] == 'X') return 16;
    if (digits[1] == 'o' || digits[1] == 'O') return 8;
    if (digits[1] == 'b' || digits[1] == 'B') return 2;
  }
  return 10;
}

clux_value_t *exec_literal_numeric(clux_context_t *ctx, node_t *node) {
  const location_t *loc = node_get_location(node);
  const char *text = loc->begin.offset;
  size_t len = (size_t)(loc->end.offset - loc->begin.offset);
  size_t num_len = len;
  const char *suffix = strip_suffix(text, len, &num_len);

  if (suffix && (strcmp(suffix, "f32") == 0 || strcmp(suffix, "f64") == 0)) {
    return make_float(ctx, suffix, text, num_len, loc);
  }

  bool negative = num_len > 0 && text[0] == '-';
  const char *digits = text + (negative ? 1 : 0);
  size_t digits_len = num_len - (size_t)(negative ? 1 : 0);
  int base = digit_base(digits, digits_len);
  if (base != 10) {
    digits += 2;
    digits_len -= 2;
  }

  char buffer[64];
  if (digits_len >= sizeof(buffer)) {
    return clux_context_create_error_message(
        ctx, (location_t *)loc, "numeric literal too long");
  }
  memcpy(buffer, digits, digits_len);
  buffer[digits_len] = '\0';
  uint64_t magnitude = strtoull(buffer, NULL, base);
  return make_int(ctx, suffix, magnitude, negative, loc);
}

/* 解码一个转义序列（与 parser 的 scan_escape 同集）：s 指向 '\\'。
   成功返回消耗字节数并写 *out；失败返回 0。 */
static size_t decode_escape(const char *s, const char *end, uint32_t *out) {
  if (s + 1 >= end) {
    return 0;
  }
  switch (s[1]) {
  case 'n':
    *out = 0x0A;
    return 2;
  case 't':
    *out = 0x09;
    return 2;
  case 'r':
    *out = 0x0D;
    return 2;
  case '\\':
    *out = '\\';
    return 2;
  case '\'':
    *out = '\'';
    return 2;
  case '"':
    *out = '"';
    return 2;
  case '0':
    *out = 0;
    return 2;
  case 'x': {
    size_t digits = 0;
    uint32_t value = 0;
    while (digits < 2 && s + 2 + digits < end) {
      char h = s[2 + digits];
      if (!is_hex_digit(h)) break;
      value = value * 16 + (uint32_t)hex_value(h);
      digits++;
    }
    if (digits == 0) {
      return 0;
    }
    *out = value;
    return 2 + digits;
  }
  default:
    return 0;
  }
}

/* 字面量内容解码（去引号）：返回分配的 NUL 结尾副本并写 *out_len，
   转义非法返回 NULL。 */
static char *
decode_content(clux_context_t *ctx, const location_t *loc, size_t *out_len) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const char *s = loc->begin.offset + 1;
  const char *end = loc->end.offset - 1;
  size_t len = (size_t)(end - s);
  char *buffer = allocator_alloc(allocator, len + 1);
  size_t used = 0;
  while (s < end) {
    uint32_t code = 0;
    size_t consumed = 1;
    if (*s == '\\') {
      consumed = decode_escape(s, end, &code);
      if (consumed == 0) {
        allocator_free(allocator, (void **)&buffer);
        return NULL;
      }
    } else {
      code = (uint32_t)(unsigned char)*s;
    }
    buffer[used++] = (char)code;
    s += consumed;
  }
  buffer[used] = '\0';
  *out_len = used;
  return buffer;
}

clux_value_t *exec_literal_string(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  size_t len = 0;
  char *text = decode_content(ctx, loc, &len);
  if (!text) {
    return clux_context_create_error_message(
        ctx, (location_t *)loc, "invalid escape sequence in string literal");
  }
  clux_value_t *value =
      clux_context_create_str(ctx, text, NULL, (location_t *)loc);
  allocator_free(allocator, (void **)&text);
  return value;
}

clux_value_t *exec_literal_charactor(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  size_t len = 0;
  char *text = decode_content(ctx, loc, &len);
  if (!text) {
    return clux_context_create_error_message(
        ctx, (location_t *)loc, "invalid escape sequence in character literal");
  }
  if (len != 1) {
    allocator_free(allocator, (void **)&text);
    return clux_context_create_error_message(
        ctx,
        (location_t *)loc,
        "character literal must contain exactly one u8 character");
  }
  uint8_t code = (uint8_t)text[0];
  allocator_free(allocator, (void **)&text);
  return clux_context_create_u8(ctx, code, NULL, (location_t *)loc);
}

clux_value_t *exec_literal_boolean(clux_context_t *ctx, node_t *node) {
  const location_t *loc = node_get_location(node);
  return clux_context_create_bool(
      ctx, location_is(loc, "true"), NULL, (location_t *)loc);
}
