#include "core/error.h"
#include "core/allocator.h"
#include "core/location.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unicode/utf8.h>

#define CLUX_DIAGNOSTIC_TAB_WIDTH 4

struct _clux_error_t {
  char *msg;
  location_t loc;
  const char *source;
  allocator_t *allocator;
};
clux_error_t *create_error(allocator_t *allocator,
                           const location_t *loc,
                           const char *fmt,
                           ...) {
  va_list args;
  va_start(args, fmt);
  size_t len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  char *msg = allocator_alloc(allocator, len + 1);
  va_start(args, fmt);
  (void)vsnprintf(msg, len + 1, fmt, args);
  va_end(args);
  clux_error_t *err = allocator_alloc(allocator, sizeof(struct _clux_error_t));
  err->msg = msg;
  if (loc) {
    err->loc = *loc;
  } else {
    memset(&err->loc, 0, sizeof(err->loc));
  }
  err->source = NULL;
  err->allocator = allocator;
  return err;
}
const location_t *error_get_location(clux_error_t *self) { return &self->loc; }
const char *error_get_message(clux_error_t *self) { return self->msg; }
void error_set_source(clux_error_t *self, const char *source) {
  self->source = source;
}
const char *error_get_source(clux_error_t *self) { return self->source; }

static size_t append_spaces(string_t *out, size_t count) {
  for (size_t i = 0; i < count; i++) {
    string_append_char(out, ' ');
  }
  return count;
}

static size_t append_size(string_t *out, size_t value) {
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%zu", value);
  if (len > 0) {
    string_append(out, buf, (size_t)len);
  }
  return (size_t)len;
}

static size_t digit_count(size_t value) {
  size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    digits++;
  }
  return digits;
}

/* 从 start 到 end 的显示宽度；制表符按 4 列对齐，UTF-8 码点按 1 列。 */
static size_t display_width(const char *start, const char *end) {
  size_t width = 0;
  const char *p = start;
  while (p < end) {
    if (*p == '\t') {
      width += CLUX_DIAGNOSTIC_TAB_WIDTH - (width % CLUX_DIAGNOSTIC_TAB_WIDTH);
      p++;
      continue;
    }
    const uint8_t *u = (const uint8_t *)p;
    int32_t i = 0;
    UChar32 c;
    U8_NEXT(u, i, (int32_t)(end - p), c);
    if (c < 0 || i <= 0) {
      i = 1;
    }
    width++;
    p += i;
  }
  return width;
}

/* 追加源码行，制表符展开为空格以保持与 caret 对齐。 */
static void
append_source_line(string_t *out, const char *start, const char *end) {
  size_t width = 0;
  const char *p = start;
  while (p < end) {
    if (*p == '\t') {
      size_t spaces =
          CLUX_DIAGNOSTIC_TAB_WIDTH - (width % CLUX_DIAGNOSTIC_TAB_WIDTH);
      append_spaces(out, spaces);
      width += spaces;
      p++;
      continue;
    }
    const uint8_t *u = (const uint8_t *)p;
    int32_t i = 0;
    UChar32 c;
    U8_NEXT(u, i, (int32_t)(end - p), c);
    if (c < 0 || i <= 0) {
      i = 1;
    }
    string_append(out, p, (size_t)i);
    width++;
    p += i;
  }
}

/* 找到 begin 所在行的起点与终点（不含换行）。 */
static void find_line_bounds(const char *source,
                             const char *begin,
                             const char **line_start,
                             const char **line_end) {
  const char *start = begin;
  while (start > source && start[-1] != '\n') {
    start--;
  }
  const char *end = begin;
  while (*end != '\0' && *end != '\n' && *end != '\r') {
    end++;
  }
  *line_start = start;
  *line_end = end;
}

static size_t line_number_of(const char *source, const char *line_start) {
  size_t line = 1;
  for (const char *p = source; p < line_start; p++) {
    if (*p == '\n') {
      line++;
    }
  }
  return line;
}

string_t *error_to_string(clux_error_t *self, allocator_t *allocator) {
  string_t *out = create_string(allocator);
  string_append_cstr(out, "error: ");
  string_append_cstr(out, self->msg);
  string_append_char(out, '\n');

  const char *filename = self->loc.filename ? self->loc.filename : "<unknown>";
  const char *source = self->source;
  const char *begin = self->loc.begin.offset;
  const char *end = self->loc.end.offset;

  bool has_source = source && begin && (uintptr_t)begin >= (uintptr_t)source &&
                    (uintptr_t)begin <= (uintptr_t)source + strlen(source);
  size_t line = 1;
  size_t column = 0;
  size_t caret_len = 1;
  const char *line_start = NULL;
  const char *line_end = NULL;

  if (has_source) {
    find_line_bounds(source, begin, &line_start, &line_end);
    line = line_number_of(source, line_start);
    column = display_width(line_start, begin);
    const char *caret_end = end;
    if (!caret_end || caret_end < begin || caret_end > line_end) {
      caret_end = line_end;
    }
    size_t end_column = display_width(line_start, caret_end);
    caret_len = end_column > column ? end_column - column : 1;
  } else {
    line = (size_t)self->loc.begin.line + 1;
    column = (size_t)self->loc.begin.column;
  }

  string_append_cstr(out, " --> ");
  string_append_cstr(out, filename);
  string_append_char(out, ':');
  append_size(out, line);
  string_append_char(out, ':');
  append_size(out, column + 1);
  string_append_char(out, '\n');

  if (!has_source) {
    return out;
  }

  size_t num_width = digit_count(line);
  append_spaces(out, num_width + 1);
  string_append_cstr(out, "|\n");

  append_size(out, line);
  string_append_cstr(out, " | ");
  append_source_line(out, line_start, line_end);
  string_append_char(out, '\n');

  append_spaces(out, num_width + 1);
  string_append_cstr(out, "| ");
  append_spaces(out, column);
  for (size_t i = 0; i < caret_len; i++) {
    string_append_char(out, '^');
  }
  string_append_char(out, '\n');
  return out;
}
void delete_error(clux_error_t **error) {
  if (!error || !*error) {
    return;
  }
  allocator_free((*error)->allocator, (void **)&(*error)->msg);
  allocator_free((*error)->allocator, (void **)error);
}