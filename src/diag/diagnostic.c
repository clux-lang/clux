#include "diag/diagnostic.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- Internal buffer layout ---- */

struct _diag_buf_t {
  allocator_t   *alloc;
  diagnostic_t  *items; /* dynamic array, grown geometrically */
  size_t         count;
  size_t         capacity;
};

static const char *diag_level_name(diag_level_t level) {
  switch (level) {
    case DIAG_ERROR: return "error";
    case DIAG_WARNING: return "warning";
    case DIAG_NOTE: return "note";
  }
  return "unknown";
}

/* ---- Growth ---- */

static void diag_grow(diag_buf_t *db) {
  size_t new_cap = db->capacity ? db->capacity * 2 : 16;
  /* The array owns only the diagnostic structs; messages are freed
     individually in diag_buf_destroy / on reallocation by hand. */
  diagnostic_t *items = allocator_new_ex(db->alloc, "diagnostic_t",
                                         sizeof(diagnostic_t), NULL, NULL,
                                         NULL, new_cap);
  if (db->items) {
    /* Ownership transfer of structs (incl. message pointers): move, not clone. */
    memcpy(items, db->items, db->count * sizeof(diagnostic_t));
    allocator_free(db->alloc, (void **)&db->items);
  }
  db->items = items;
  db->capacity = new_cap;
}

/* ---- Lifecycle ---- */

diag_buf_t *diag_buf_new(allocator_t *alloc) {
  if (!alloc) return NULL;
  diag_buf_t *db =
      allocator_new_ex(alloc, "diag_buf_t", sizeof(diag_buf_t), NULL, NULL,
                       NULL, 1);
  db->alloc = alloc;
  db->items = NULL;
  db->count = 0;
  db->capacity = 0;
  return db;
}

void diag_buf_destroy(diag_buf_t **db) {
  if (!db || !*db) return;
  diag_buf_t *self = *db;
  for (size_t i = 0; i < self->count; i++) {
    allocator_free(self->alloc, (void **)&self->items[i].message);
    allocator_free(self->alloc, (void **)&self->items[i].filename);
  }
  allocator_free(self->alloc, (void **)&self->items);
  allocator_free(self->alloc, (void **)db);
}

/* ---- Recording ---- */

static void diag_add(diag_buf_t *db, diag_level_t level, location_t loc,
                     const char *fmt, va_list ap) {
  if (!db) return;
  if (db->count == db->capacity) diag_grow(db);
  diagnostic_t *d = &db->items[db->count];
  d->level = level;
  d->loc = loc;
  d->message = NULL;
  d->filename = NULL;
  if (loc.filename) {
    /* 位置里的 filename 是借用指针，注册时拷贝一份自有副本，
       使诊断在调用方释放路径缓冲后仍可打印（含源码片段）。 */
    size_t n = strlen(loc.filename) + 1;
    d->filename = allocator_new_ex(db->alloc, "char", sizeof(char),
                                   default_move, default_clone, NULL, n);
    memcpy(d->filename, loc.filename, n);
  }
  if (fmt) {
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (len >= 0) {
      size_t cap = (size_t)len + 1;
      d->message = allocator_new_ex(db->alloc, "char", sizeof(char),
                                    default_move, default_clone, NULL, cap);
      vsnprintf(d->message, cap, fmt, ap);
    }
  }
  db->count++;
}

void diag_error(diag_buf_t *db, location_t loc, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_add(db, DIAG_ERROR, loc, fmt, ap);
  va_end(ap);
}

void diag_warning(diag_buf_t *db, location_t loc, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_add(db, DIAG_WARNING, loc, fmt, ap);
  va_end(ap);
}

void diag_note(diag_buf_t *db, location_t loc, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_add(db, DIAG_NOTE, loc, fmt, ap);
  va_end(ap);
}

/* ---- Queries / output ---- */

size_t diag_count(const diag_buf_t *db) { return db ? db->count : 0; }

bool diag_has_error(const diag_buf_t *db) {
  if (!db) return false;
  for (size_t i = 0; i < db->count; i++) {
    if (db->items[i].level == DIAG_ERROR) return true;
  }
  return false;
}

/* ---- Rust 风格源码片段输出 ---- */

/**
 * 读取 filename 第 line_no 行（1-based）到栈缓冲，返回该行去行尾后的
 * 长度；文件无法打开 / 行越界 / 行为空返回 0。行超长时截断（M1 可接受）。
 */
static size_t read_source_line(const char *filename, size_t line_no,
                               char *buf, size_t cap) {
  FILE *fp = fopen(filename, "rb");
  if (!fp) return 0;

  size_t cur = 1;
  while (cur < line_no && fgets(buf, cap, fp)) {
    /* 行被截断（无 \n）：跳过本行剩余字符，保持行号计数正确 */
    if (!strchr(buf, '\n')) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') {}
    }
    cur++;
  }

  size_t len = 0;
  if (cur == line_no && fgets(buf, cap, fp)) {
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
      buf[--len] = '\0';
  }
  fclose(fp);
  return len;
}

/** 返回 1-based 行号打印占位宽度（至少 1） */
static size_t line_no_width(size_t line_no) {
  size_t w = 1;
  for (size_t n = line_no; n >= 10; n /= 10) w++;
  return w;
}

/**
 * 打印 Rust 风格诊断块（含源码行与 ^ 标记）：
 *
 *   error: expects 1 arguments, got 2
 *    --> s1.clx:1:18
 *     |
 *   1 | func main() { foo(1, 2); }
 *     |                  ^
 *     |
 *
 * 源码行读取失败（文件不存在 / 行越界 / 空行）返回 false，由调用方
 * fallback 到单行格式。
 */
static bool print_snippet_block(FILE *out, const diagnostic_t *d) {
  const char *filename = d->filename;
  size_t line_no = d->loc.begin.line;
  size_t col = d->loc.begin.column;
  if (!filename || line_no == 0 || col == 0) return false;

  char line[2048];
  size_t len = read_source_line(filename, line_no, line, sizeof line);
  if (len == 0) return false;

  size_t w = line_no_width(line_no);

  /* 1. 级别 + 消息 */
  fprintf(out, "%s: %s\n", diag_level_name(d->level),
          d->message ? d->message : "");
  /* 2. 位置 */
  fprintf(out, "%*s--> %s:%zu:%zu\n", (int)w, "", filename, line_no, col);
  /* 3. 顶部分隔 */
  fprintf(out, "%*s|\n", (int)(w + 1), "");
  /* 4. 源码行（右对齐行号） */
  fprintf(out, "%*zu | %.*s\n", (int)w, line_no, (int)len, line);
  /* 5. caret：同行 end 范围，至少 1，不越出源码行 */
  size_t caret_len = 1;
  if (d->loc.end.line == line_no && d->loc.end.column > col)
    caret_len = d->loc.end.column - col;
  if (col - 1 + caret_len > len) caret_len = 1;
  fprintf(out, "%*s| %*s", (int)(w + 1), "", (int)(col - 1), "");
  for (size_t i = 0; i < caret_len; i++) fputc('^', out);
  fputc('\n', out);
  /* 6. 底部收尾 */
  fprintf(out, "%*s|\n", (int)(w + 1), "");
  return true;
}

void diag_print_all(const diag_buf_t *db) {
  if (!db) return;
  for (size_t i = 0; i < db->count; i++) {
    const diagnostic_t *d = &db->items[i];
    const char *file = d->filename ? d->filename : "<unknown>";

    /* 能读到源码行 → Rust 风格块；否则 fallback 单行 */
    if (print_snippet_block(stderr, d)) continue;

    fprintf(stderr, "%s:%zu:%zu: %s: %s\n", file, d->loc.begin.line,
            d->loc.begin.column, diag_level_name(d->level),
            d->message ? d->message : "");
  }
}

const diagnostic_t *diag_items(const diag_buf_t *db) {
  if (!db || db->count == 0) return NULL;
  return db->items;
}
