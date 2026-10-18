#include "core/ffi_cond.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * 进程级**全局静态表**（无堆分配；见 header 说明）
 * =========================================================================== */

typedef struct {
  char key[FFI_DEFINES_KEY_MAX];
  char value[FFI_DEFINES_VALUE_MAX];
} ffi_define_t;

static ffi_define_t g_defines[FFI_DEFINES_MAX];
static size_t g_define_count = 0;
static bool g_defines_ready = false; /* 内建键是否已写入 */

static bool is_ident_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static bool is_ident_char(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}
static bool is_ident(const char *s) {
  if (!s || !is_ident_start(*s)) return false;
  for (const char *p = s + 1; *p; p++) {
    if (!is_ident_char(*p)) return false;
  }
  return true;
}

static int defines_put(const char *key, const char *value) {
  if (!is_ident(key) || strlen(key) >= FFI_DEFINES_KEY_MAX) return 1;
  const char *v = value ? value : "";
  if (strlen(v) >= FFI_DEFINES_VALUE_MAX) return 1;
  for (size_t i = 0; i < g_define_count; i++) { /* 覆盖 */
    if (strcmp(g_defines[i].key, key) == 0) {
      snprintf(g_defines[i].value, sizeof g_defines[i].value, "%s", v);
      return 0;
    }
  }
  if (g_define_count >= FFI_DEFINES_MAX) return 1;
  snprintf(g_defines[g_define_count].key, sizeof g_defines[g_define_count].key,
           "%s", key);
  snprintf(g_defines[g_define_count].value,
           sizeof g_defines[g_define_count].value, "%s", v);
  g_define_count++;
  return 0;
}

void ffi_defines_add_builtins(void) {
#if defined(_WIN32)
  defines_put("OS_TYPE", sizeof(void *) == 8 ? "WIN64" : "WIN32");
#elif defined(__APPLE__)
  defines_put("OS_TYPE", "MACOS");
#elif defined(__linux__)
  defines_put("OS_TYPE", "LINUX");
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  defines_put("OS_TYPE", "BSD");
#else
  defines_put("OS_TYPE", "UNKNOWN");
#endif
#if defined(__x86_64__) || defined(_M_X64)
  defines_put("OS_ARCH", "X64");
#elif defined(__aarch64__) || defined(_M_ARM64)
  defines_put("OS_ARCH", "ARM64");
#elif defined(__i386__) || defined(_M_IX86)
  defines_put("OS_ARCH", "X86");
#elif defined(__arm__) || defined(_M_ARM)
  defines_put("OS_ARCH", "ARM");
#else
  defines_put("OS_ARCH", "UNKNOWN");
#endif
  g_defines_ready = true;
}

void ffi_defines_reset(void) {
  g_define_count = 0;
  g_defines_ready = false;
  ffi_defines_add_builtins();
}

static void defines_ensure_ready(void) {
  if (!g_defines_ready) ffi_defines_add_builtins();
}

int ffi_defines_add(const char *key, const char *value) {
  defines_ensure_ready();
  return defines_put(key, value);
}

int ffi_defines_add_kv(const char *kv) {
  if (!kv || !*kv) return 1;
  const char *eq = strchr(kv, '=');
  if (!eq || eq == kv) return 1; /* 必须 KEY=VALUE，且 KEY 非空 */
  size_t klen = (size_t)(eq - kv);
  if (klen >= FFI_DEFINES_KEY_MAX) return 1;
  char key[FFI_DEFINES_KEY_MAX];
  memcpy(key, kv, klen);
  key[klen] = '\0';
  return ffi_defines_add(key, eq + 1);
}

const char *ffi_defines_get(const char *key) {
  if (!key) return NULL;
  defines_ensure_ready();
  for (size_t i = 0; i < g_define_count; i++) {
    if (strcmp(g_defines[i].key, key) == 0) return g_defines[i].value;
  }
  return NULL;
}

size_t ffi_defines_count(void) { return g_define_count; }

const char *ffi_defines_key_at(size_t i) {
  if (i >= g_define_count) return NULL;
  return g_defines[i].key;
}

/* ===========================================================================
 * 条件语言：词法 + 递归下降求值（文法见 header）
 * =========================================================================== */

typedef enum {
  FFI_TOK_END = 0,
  FFI_TOK_IDENT,
  FFI_TOK_STRING,
  FFI_TOK_EQ,
  FFI_TOK_NE,
  FFI_TOK_AND,
  FFI_TOK_OR,
  FFI_TOK_LPAREN,
  FFI_TOK_RPAREN,
  FFI_TOK_ERROR,
} ffi_tok_t;

typedef struct {
  ffi_tok_t kind;
  const char *begin; /* IDENT / STRING 的内容起点 */
  size_t len;
  size_t pos; /* 起始偏移（诊断用） */
} ffi_token_t;

typedef struct {
  const char *text;
  size_t len;
  size_t pos;
  ffi_token_t cur;
  bool check_keys; /* false = 只查语法（不查键存在性） */
  char *err;
  size_t ecap;
  bool failed;
} ffi_parse_t;

static void cond_err(ffi_parse_t *p, size_t pos, const char *fmt, ...) {
  if (p->failed) return; /* 首个错误优先 */
  p->failed = true;
  if (!p->err || p->ecap == 0) return;
  size_t used = 0;
  int n = snprintf(p->err, p->ecap, "assert: ");
  if (n > 0) used = ((size_t)n < p->ecap) ? (size_t)n : p->ecap - 1;
  va_list ap;
  va_start(ap, fmt);
  if (used + 1 < p->ecap) vsnprintf(p->err + used, p->ecap - used, fmt, ap);
  va_end(ap);
  size_t l = strlen(p->err);
  if (l + 24 < p->ecap)
    snprintf(p->err + l, p->ecap - l, " (at offset %zu)", pos);
}

static void skip_ws(ffi_parse_t *p) {
  while (p->pos < p->len) {
    char c = p->text[p->pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      p->pos++;
    else
      break;
  }
}

static void next_token(ffi_parse_t *p) {
  skip_ws(p);
  ffi_token_t t = {FFI_TOK_END, NULL, 0, p->pos};
  if (p->pos >= p->len) {
    p->cur = t;
    return;
  }
  const char *s = p->text + p->pos;
  size_t remain = p->len - p->pos;
  if (is_ident_start(s[0])) {
    size_t n = 0;
    while (n < remain && is_ident_char(s[n])) n++;
    t.kind = FFI_TOK_IDENT;
    t.begin = s;
    t.len = n;
    p->pos += n;
  } else if (s[0] == '"') {
    size_t n = 1;
    while (n < remain && s[n] != '"') n++;
    if (n >= remain) {
      t.kind = FFI_TOK_ERROR;
      cond_err(p, t.pos, "unterminated string literal");
    } else {
      t.kind = FFI_TOK_STRING;
      t.begin = s + 1;
      t.len = n - 1;
      p->pos += n + 1;
    }
  } else if (remain >= 2 && s[0] == '=' && s[1] == '=') {
    t.kind = FFI_TOK_EQ;
    p->pos += 2;
  } else if (remain >= 2 && s[0] == '!' && s[1] == '=') {
    t.kind = FFI_TOK_NE;
    p->pos += 2;
  } else if (remain >= 2 && s[0] == '&' && s[1] == '&') {
    t.kind = FFI_TOK_AND;
    p->pos += 2;
  } else if (remain >= 2 && s[0] == '|' && s[1] == '|') {
    t.kind = FFI_TOK_OR;
    p->pos += 2;
  } else if (s[0] == '(') {
    t.kind = FFI_TOK_LPAREN;
    p->pos += 1;
  } else if (s[0] == ')') {
    t.kind = FFI_TOK_RPAREN;
    p->pos += 1;
  } else {
    t.kind = FFI_TOK_ERROR;
    cond_err(p, t.pos, "unexpected character '%c'", s[0]);
  }
  p->cur = t;
}

static bool parse_or(ffi_parse_t *p, bool *out);

/** 比较项：`( cond )` 或 `KEY (==|!=) "value"`。 */
static bool parse_cmp(ffi_parse_t *p, bool *out) {
  if (p->failed) return false;
  if (p->cur.kind == FFI_TOK_LPAREN) {
    next_token(p);
    if (!parse_or(p, out)) return false;
    if (p->cur.kind != FFI_TOK_RPAREN) {
      cond_err(p, p->cur.pos, "expected ')'");
      return false;
    }
    next_token(p);
    return true;
  }
  if (p->cur.kind != FFI_TOK_IDENT) {
    cond_err(p, p->cur.pos, "expected a key name or '('");
    return false;
  }
  char key[FFI_DEFINES_KEY_MAX];
  size_t klen = p->cur.len < sizeof(key) - 1 ? p->cur.len : sizeof(key) - 1;
  memcpy(key, p->cur.begin, klen);
  key[klen] = '\0';
  size_t key_pos = p->cur.pos;
  next_token(p);
  if (p->cur.kind != FFI_TOK_EQ && p->cur.kind != FFI_TOK_NE) {
    cond_err(p, p->cur.pos, "expected '==' or '!=' after key '%s'", key);
    return false;
  }
  bool want_equal = (p->cur.kind == FFI_TOK_EQ);
  next_token(p);
  if (p->cur.kind != FFI_TOK_STRING) {
    cond_err(p, p->cur.pos, "expected a string literal after '%s'",
             want_equal ? "==" : "!=");
    return false;
  }
  char value[FFI_DEFINES_VALUE_MAX];
  size_t vlen = p->cur.len < sizeof(value) - 1 ? p->cur.len : sizeof(value) - 1;
  memcpy(value, p->cur.begin, vlen);
  value[vlen] = '\0';
  next_token(p);

  if (!p->check_keys) { /* 只查语法：值不参与 */
    *out = true;
    return true;
  }
  const char *actual = ffi_defines_get(key);
  if (!actual) {
    cond_err(p, key_pos,
             "unknown define '%s' (inject it with -D %s=<value>)", key, key);
    return false;
  }
  bool equal = strcmp(actual, value) == 0;
  *out = want_equal ? equal : !equal;
  return true;
}

static bool parse_and(ffi_parse_t *p, bool *out) {
  bool lhs = false;
  if (!parse_cmp(p, &lhs)) return false;
  while (p->cur.kind == FFI_TOK_AND && !p->failed) {
    next_token(p);
    bool rhs = false;
    if (!parse_cmp(p, &rhs)) return false;
    lhs = lhs && rhs;
  }
  *out = lhs;
  return true;
}

static bool parse_or(ffi_parse_t *p, bool *out) {
  bool lhs = false;
  if (!parse_and(p, &lhs)) return false;
  while (p->cur.kind == FFI_TOK_OR && !p->failed) {
    next_token(p);
    bool rhs = false;
    if (!parse_and(p, &rhs)) return false;
    lhs = lhs || rhs;
  }
  *out = lhs;
  return true;
}

static bool cond_run(const char *text, bool check_keys, bool *out, char *err,
                     size_t ecap) {
  if (err && ecap) err[0] = '\0';
  if (out) *out = false;
  if (!text || !*text) {
    if (err && ecap) snprintf(err, ecap, "assert: empty condition");
    return false;
  }
  ffi_parse_t p;
  memset(&p, 0, sizeof p);
  p.text = text;
  p.len = strlen(text);
  p.check_keys = check_keys;
  p.err = err;
  p.ecap = ecap;
  next_token(&p);
  bool result = false;
  bool ok = parse_or(&p, &result);
  if (ok && !p.failed && p.cur.kind != FFI_TOK_END) {
    cond_err(&p, p.cur.pos, "unexpected trailing token");
    ok = false;
  }
  if (!ok || p.failed) return false;
  if (out) *out = result;
  return true;
}

bool ffi_cond_validate(const char *text, char *err, size_t ecap) {
  return cond_run(text, /*check_keys=*/false, NULL, err, ecap);
}

bool ffi_cond_eval(const char *text, bool *out, char *err, size_t ecap) {
  return cond_run(text, /*check_keys=*/true, out, err, ecap);
}
