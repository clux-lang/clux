/* literal 求值：AST 节点只记 location 区间，文本由这里从源码解码。
   数字支持 0x/0o/0b/十进制与 i8..f64/isize/usize 后缀（后缀决定类型，
   无后缀整数默认 i32；浮点后缀由语法层强制要求，见 literal.c 的
   validate_numeric_literal）。字符串/字符走与 parser 相同的转义集。 */
#include "exec_internal.h"
#include <stdlib.h>
#include <string.h>

exec_value_t exec_visit_undefined(clux_exec_t *self, node_t *node) {
  return exec_errorf(
      self, exec_loc(node), "undefined literal is not executable");
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
  static const char *suffixes[] = {"isize", "usize", "i8",  "i16", "i32",
                                   "i64",   "u8",    "u16", "u32", "u64",
                                   "f32",   "f64",   NULL};
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

static exec_value_t make_int(clux_exec_t *self,
                             const char *suffix,
                             uint64_t magnitude,
                             bool negative,
                             const location_t *loc) {
  clux_context_t *ctx = self->ctx;
  int64_t signed_value = negative ? -(int64_t)magnitude : (int64_t)magnitude;
  if (!suffix) {
    return exec_ok(clux_context_create_i32(ctx, (int32_t)signed_value, NULL, NULL));
  }
  if (strcmp(suffix, "i8") == 0)
    return exec_ok(
        clux_context_create_i8(ctx, (int8_t)signed_value, NULL, NULL));
  if (strcmp(suffix, "i16") == 0)
    return exec_ok(
        clux_context_create_i16(ctx, (int16_t)signed_value, NULL, NULL));
  if (strcmp(suffix, "i32") == 0)
    return exec_ok(
        clux_context_create_i32(ctx, (int32_t)signed_value, NULL, NULL));
  if (strcmp(suffix, "i64") == 0)
    return exec_ok(clux_context_create_i64(ctx, signed_value, NULL, NULL));
  if (strcmp(suffix, "isize") == 0)
    return exec_ok(
        clux_context_create_isize(ctx, (intptr_t)signed_value, NULL, NULL));
  if (strcmp(suffix, "u8") == 0)
    return exec_ok(clux_context_create_u8(ctx, (uint8_t)magnitude, NULL, NULL));
  if (strcmp(suffix, "u16") == 0)
    return exec_ok(
        clux_context_create_u16(ctx, (uint16_t)magnitude, NULL, NULL));
  if (strcmp(suffix, "u32") == 0)
    return exec_ok(
        clux_context_create_u32(ctx, (uint32_t)magnitude, NULL, NULL));
  if (strcmp(suffix, "u64") == 0)
    return exec_ok(clux_context_create_u64(ctx, magnitude, NULL, NULL));
  if (strcmp(suffix, "usize") == 0)
    return exec_ok(
        clux_context_create_usize(ctx, (size_t)magnitude, NULL, NULL));
  return exec_errorf(self, loc, "invalid integer suffix '%s'", suffix);
}

static exec_value_t make_float(clux_exec_t *self,
                               const char *suffix,
                               const char *digits,
                               size_t digits_len,
                               const location_t *loc) {
  char buffer[64];
  if (digits_len >= sizeof(buffer)) {
    return exec_errorf(self, loc, "numeric literal too long");
  }
  memcpy(buffer, digits, digits_len);
  buffer[digits_len] = '\0';
  if (strcmp(suffix, "f32") == 0) {
    return exec_ok(
        clux_context_create_f32(self->ctx, strtof(buffer, NULL), NULL, NULL));
  }
  if (strcmp(suffix, "f64") == 0) {
    return exec_ok(
        clux_context_create_f64(self->ctx, strtod(buffer, NULL), NULL, NULL));
  }
  return exec_errorf(self, loc, "invalid float suffix '%s'", suffix);
}

static int digit_base(const char *digits, size_t len) {
  if (len > 2 && digits[0] == '0') {
    if (digits[1] == 'x' || digits[1] == 'X') return 16;
    if (digits[1] == 'o' || digits[1] == 'O') return 8;
    if (digits[1] == 'b' || digits[1] == 'B') return 2;
  }
  return 10;
}

exec_value_t exec_visit_numeric(clux_exec_t *self, node_t *node) {
  const location_t *loc = exec_loc(node);
  const char *text = loc->begin.offset;
  size_t len = (size_t)(loc->end.offset - loc->begin.offset);
  size_t num_len = len;
  const char *suffix = strip_suffix(text, len, &num_len);

  if (suffix && (strcmp(suffix, "f32") == 0 || strcmp(suffix, "f64") == 0)) {
    return make_float(self, suffix, text, num_len, loc);
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
    return exec_errorf(self, loc, "numeric literal too long");
  }
  memcpy(buffer, digits, digits_len);
  buffer[digits_len] = '\0';
  uint64_t magnitude = strtoull(buffer, NULL, base);
  return make_int(self, suffix, magnitude, negative, loc);
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
static char *decode_content(clux_exec_t *self,
                            const location_t *loc,
                            size_t *out_len) {
  const char *s = loc->begin.offset + 1;
  const char *end = loc->end.offset - 1;
  size_t len = (size_t)(end - s);
  char *buffer = allocator_alloc(self->allocator, len + 1);
  size_t used = 0;
  while (s < end) {
    uint32_t code = 0;
    size_t consumed = 1;
    if (*s == '\\') {
      consumed = decode_escape(s, end, &code);
      if (consumed == 0) {
        allocator_free(self->allocator, (void **)&buffer);
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

exec_value_t exec_visit_string(clux_exec_t *self, node_t *node) {
  const location_t *loc = exec_loc(node);
  size_t len = 0;
  char *text = decode_content(self, loc, &len);
  if (!text) {
    return exec_errorf(self, loc, "invalid escape sequence in string literal");
  }
  clux_value_t *value =
      clux_context_create_str(self->ctx, text, NULL, NULL);
  allocator_free(self->allocator, (void **)&text);
  return exec_ok(value);
}

exec_value_t exec_visit_character(clux_exec_t *self, node_t *node) {
  const location_t *loc = exec_loc(node);
  size_t len = 0;
  char *text = decode_content(self, loc, &len);
  if (!text) {
    return exec_errorf(self, loc, "invalid escape sequence in character literal");
  }
  if (len != 1) {
    allocator_free(self->allocator, (void **)&text);
    return exec_errorf(
        self, loc, "character literal must contain exactly one u8 character");
  }
  uint8_t code = (uint8_t)text[0];
  allocator_free(self->allocator, (void **)&text);
  return exec_ok(clux_context_create_u8(self->ctx, code, NULL, NULL));
}

exec_value_t exec_visit_boolean(clux_exec_t *self, node_t *node) {
  return exec_ok(clux_context_create_bool(
      self->ctx, exec_node_is(node, "true"), NULL, NULL));
}

exec_value_t exec_visit_identifier(clux_exec_t *self, node_t *node) {
  char *name = exec_node_text(self, node);
  clux_value_t *value = clux_scope_get(clux_context_get_scope(self->ctx), name);
  if (!value) {
    exec_value_t error = exec_errorf(
        self, exec_loc(node), "undefined name '%s'", name);
    allocator_free(self->allocator, (void **)&name);
    return error;
  }
  allocator_free(self->allocator, (void **)&name);
  return exec_ok(value);
}

/* 内建类型名 -> type value（语法层类型就是普通表达式，白皮书 §2.1）。 */
exec_value_t exec_visit_type_literal(clux_exec_t *self, node_t *node) {
  clux_context_t *ctx = self->ctx;
  const struct {
    const char *name;
    clux_type_t *(*get)(clux_context_t *);
  } table[] = {
      {"i8", clux_context_get_i8_type},
      {"i16", clux_context_get_i16_type},
      {"i32", clux_context_get_i32_type},
      {"i64", clux_context_get_i64_type},
      {"u8", clux_context_get_u8_type},
      {"u16", clux_context_get_u16_type},
      {"u32", clux_context_get_u32_type},
      {"u64", clux_context_get_u64_type},
      {"isize", clux_context_get_isize_type},
      {"usize", clux_context_get_usize_type},
      {"f32", clux_context_get_f32_type},
      {"f64", clux_context_get_f64_type},
      {"bool", clux_context_get_bool_type},
      {"str", clux_context_get_str_type},
  };
  for (size_t idx = 0; idx < sizeof(table) / sizeof(table[0]); idx++) {
    if (exec_node_is(node, table[idx].name)) {
      return exec_ok(clux_context_create_type_value(
          ctx, table[idx].get(ctx), NULL, NULL));
    }
  }
  char *name = exec_node_text(self, node);
  exec_value_t error = exec_errorf(
      self, exec_loc(node), "type '%s' is not executable", name);
  allocator_free(self->allocator, (void **)&name);
  return error;
}
