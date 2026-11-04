#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/location.h"
#include "core/position.h"
#include <string.h>
#include <unicode/utf8.h>
#include <unicode/uchar.h>

/* 从 position 读取一个 UTF-8 码点但不推进 position，返回码点或 -1（EOF/非法）。 */
static UChar32 peek_codepoint(const position_t *pos) {
  if (*pos->offset == '\0') return -1;
  const uint8_t *s = (const uint8_t *)pos->offset;
  int32_t i = 0;
  UChar32 c;
  U8_NEXT(s, i, -1, c);
  return c;
}

/* 解析转义序列，返回码点值（>=0）或 -1 表示非法转义。
   advance 推进 position 越过转义序列（不含起始反斜杠，调用方已消费）。 */
static UChar32 parse_escape(position_t *pos) {
  /* *pos->offset 是反斜杠后的第一个字符 */
  char c = *pos->offset;
  UChar32 result;
  switch (c) {
  case 'n':
    result = '\n';
    position_next(pos);
    break;
  case 't':
    result = '\t';
    position_next(pos);
    break;
  case 'r':
    result = '\r';
    position_next(pos);
    break;
  case '0':
    result = '\0';
    position_next(pos);
    break;
  case '\\':
    result = '\\';
    position_next(pos);
    break;
  case '\'':
    result = '\'';
    position_next(pos);
    break;
  case '"':
    result = '"';
    position_next(pos);
    break;
  case 'x': {
    /* \xHH: 最多两个十六进制位 */
    position_next(pos); /* 越过 'x' */
    result = 0;
    int digits = 0;
    while (digits < 2) {
      char h = *pos->offset;
      int v;
      if (h >= '0' && h <= '9')
        v = h - '0';
      else if (h >= 'a' && h <= 'f')
        v = h - 'a' + 10;
      else if (h >= 'A' && h <= 'F')
        v = h - 'A' + 10;
      else
        break;
      result = (result << 4) | v;
      position_next(pos);
      digits++;
    }
    if (digits == 0) return -1;
    break;
  }
  default:
    return -1;
  }
  return result;
}

token_t *lexer_read_character(allocator_t *allocator,
                              position_t *position,
                              const char *filename) {
  if (*position->offset != '\'') {
    return NULL;
  }
  token_t *token = allocator_alloc(allocator, sizeof(struct _token_t));
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* 越过开头的 '\'' */

  /* 解析字符内容：普通 UTF-8 字符或转义序列 */
  if (*current.offset == '\\') {
    position_next(&current); /* 越过反斜杠 */
    if (parse_escape(&current) < 0) {
      goto onerror;
    }
  } else if (*current.offset == '\'') {
    /* 空字符字面量 '' 非法 */
    goto onerror;
  } else if (*current.offset == '\0') {
    /* 未闭合 */
    goto onerror;
  } else {
    /* 普通UTF-8字符，步进一个码点 */
    if (!position_next(&current)) {
      goto onerror;
    }
  }

  /* 期望闭合的 '\'' */
  if (*current.offset != '\'') {
    goto onerror;
  }
  position_next(&current); /* 越过闭合 '\'' */

  token->kind = CLUX_TOKEN_CHARACTER;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;

onerror:
  token->kind = CLUX_TOKEN_ERROR;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;
}

/* ---- string ----
   "..." — 支持转义序列（复用 parse_escape），空串 "" 合法。
   未闭合或非法转义返回 CLUX_TOKEN_ERROR。 */
token_t *lexer_read_string(allocator_t *allocator,
                           position_t *position,
                           const char *filename) {
  if (*position->offset != '"') return NULL;
  token_t *token = allocator_alloc(allocator, sizeof(struct _token_t));
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* 越过开头的 '"' */

  while (1) {
    char c = *current.offset;
    if (c == '\0') goto onerror; /* 未闭合 */
    if (c == '"') break;         /* 闭合 */
    if (c == '\\') {
      position_next(&current); /* 越过反斜杠 */
      if (parse_escape(&current) < 0) goto onerror;
    } else {
      if (!position_next(&current)) goto onerror;
    }
  }
  position_next(&current); /* 越过闭合 '"' */

  token->kind = CLUX_TOKEN_STRING;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;

onerror:
  token->kind = CLUX_TOKEN_ERROR;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;
}

static const char *keywords[] = {
    "var",    "func",    "import", "export",   "if",        "else",
    "for",    "foreach", "while",  "do",       "break",     "continue",
    "return", "using",   "const",  "volatile", "bool",      "i8",
    "i16",    "i32",     "i64",    "u8",       "u16",       "u32",
    "u64",    "isize",   "usize",  "f32",      "f64",       "str",
    "struct", "union",   "cunion", "enum",     "undefined", "none",
    NULL,
};

/* ---- identifier ----
   ID_Start 开头，后跟零或多个 ID_Continue 字符（Unicode 属性）。
   非标识符开头返回 NULL。 */
token_t *lexer_read_identifier(allocator_t *allocator,
                               position_t *position,
                               const char *filename) {
  UChar32 c = peek_codepoint(position);
  /* u_isIDStart 遵循 UAX#31 不含下划线，C 语言传统允许 _ 开头 */
  if (c < 0 || !(c == '_' || u_isIDStart(c))) return NULL;
  token_t *token = allocator_alloc(allocator, sizeof(struct _token_t));
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* 消费首字符 */

  while (1) {
    c = peek_codepoint(&current);
    if (c < 0 || !u_isIDPart(c)) break;
    position_next(&current);
  }
  token->kind = CLUX_TOKEN_IDENTIFIER;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  for (size_t idx = 0; keywords[idx] != NULL; idx++) {
    if (location_is(&token->loc, keywords[idx])) {
      token->kind = CLUX_TOKEN_KEYWORD;
      break;
    }
  }
  *position = current;
  return token;
}

/* ---- whitespace ----
   连续的 Unicode Whitespace 字符。非空白开头返回 NULL。 */
token_t *lexer_read_whitespace(allocator_t *allocator,
                               position_t *position,
                               const char *filename) {
  UChar32 c = peek_codepoint(position);
  if (c < 0 || !u_isWhitespace(c)) return NULL;
  token_t *token = allocator_alloc(allocator, sizeof(struct _token_t));
  position_t begin = *position;
  position_t current = *position;

  while (1) {
    c = peek_codepoint(&current);
    if (c < 0 || !u_isWhitespace(c)) break;
    position_next(&current);
  }

  token->kind = CLUX_TOKEN_WHITESPACE;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;
}

/* ---- comment ----
   // 单行注释，读到 '\n' 前（不含换行）或 EOF。
   非注释开头返回 NULL。 */
token_t *lexer_read_comment(allocator_t *allocator,
                            position_t *position,
                            const char *filename) {
  if (position->offset[0] != '/' || position->offset[1] != '/') return NULL;
  token_t *token = allocator_alloc(allocator, sizeof(struct _token_t));
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* 第一个 '/' */
  position_next(&current); /* 第二个 '/' */

  while (*current.offset != '\0' && *current.offset != '\n') {
    position_next(&current);
  }
  /* 不消费换行，留给下一个 token */

  token->kind = CLUX_TOKEN_COMMENT;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;
}

/* ---- multiline comment ----
   斜杠星 ... 星斜杠，C 风格不嵌套。未闭合返回 ERROR。
   注释内遇到星斜杠闭合，否则读到 EOF 报错。 */
token_t *lexer_read_multiline_comment(allocator_t *allocator,
                                      position_t *position,
                                      const char *filename) {
  if (position->offset[0] != '/' || position->offset[1] != '*') return NULL;
  token_t *token = allocator_alloc(allocator, sizeof(struct _token_t));
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* '/' */
  position_next(&current); /* '*' */

  while (1) {
    if (*current.offset == '\0') goto onerror; /* 未闭合 */
    if (current.offset[0] == '*' && current.offset[1] == '/') {
      position_next(&current); /* '*' */
      position_next(&current); /* '/' */
      break;
    }
    position_next(&current);
  }

  token->kind = CLUX_TOKEN_MULTILINE_COMMENT;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;

onerror:
  token->kind = CLUX_TOKEN_ERROR;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;
}

/* ---- numeric ----
   C 风格数字字面量：
     十六进制: 0x[0-9a-fA-F]+
     八进制:   0[0-7]*
     十进制:   [0-9]+
     浮点:     [0-9]+.[0-9]*([eE][+-]?[0-9]+)?
              .[0-9]+([eE][+-]?[0-9]+)?
   非数字开头返回 NULL。 */
token_t *lexer_read_numeric(allocator_t *allocator,
                            position_t *position,
                            const char *filename) {
  char c0 = position->offset[0];
  if (!((c0 >= '0' && c0 <= '9') || c0 == '.')) return NULL;
  /* 单独的 '.' 不是数字 */
  if (c0 == '.' && !(position->offset[1] >= '0' && position->offset[1] <= '9'))
    return NULL;

  token_t *token = allocator_alloc(allocator, sizeof(struct _token_t));
  position_t begin = *position;
  position_t current = *position;

  if (c0 == '0' && (position->offset[1] == 'x' || position->offset[1] == 'X')) {
    /* 十六进制 */
    position_next(&current); /* '0' */
    position_next(&current); /* 'x'/'X' */
    int digits = 0;
    while (1) {
      char h = *current.offset;
      if ((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
          (h >= 'A' && h <= 'F')) {
        position_next(&current);
        digits++;
      } else {
        break;
      }
    }
    if (digits == 0) goto onerror; /* 0x 后无数字 */
  } else if (c0 == '0' && position->offset[1] >= '0' &&
             position->offset[1] <= '7') {
    /* 八进制 */
    while (*current.offset >= '0' && *current.offset <= '7') {
      position_next(&current);
    }
  } else {
    /* 十进制 / 浮点 */
    while (*current.offset >= '0' && *current.offset <= '9') {
      position_next(&current);
    }
    /* 小数部分 */
    if (*current.offset == '.') {
      position_next(&current); /* '.' */
      while (*current.offset >= '0' && *current.offset <= '9') {
        position_next(&current);
      }
    }
    /* 指数部分 */
    char e = *current.offset;
    if (e == 'e' || e == 'E') {
      position_t save = current;
      position_next(&current); /* 'e'/'E' */
      if (*current.offset == '+' || *current.offset == '-') {
        position_next(&current);
      }
      if (!(*current.offset >= '0' && *current.offset <= '9')) {
        /* 回退：e 后无数字，不算指数 */
        current = save;
      } else {
        while (*current.offset >= '0' && *current.offset <= '9') {
          position_next(&current);
        }
      }
    }
  }

  token->kind = CLUX_TOKEN_NUMERIC;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;

onerror:
  token->kind = CLUX_TOKEN_ERROR;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;
}

static const char *symbols[] = {
    "...", "||=", "&&=", ">>=", "<<=", "==", "!=", ">=", "<=", ">>",
    "<<",  "&&",  "||",  ".?",  ".!",  "+=", "-=", "/=", "*=", "%=",
    "&=",  "|=",  "~=",  "^=",  "->",  "[[", "]]", "+",  "-",  "*",
    "/",   "%",   "&",   "|",   "!",   "=",  "(",  ")",  "[",  "]",
    "{",   "}",   "~",   "^",   ".",   ",",  "?",  ":",  ";",  NULL};

token_t *lexer_read_symbol(allocator_t *allocator,
                           position_t *position,
                           const char *filename) {
  position_t begin = *position;
  position_t current = *position;
  size_t idx = 0;
  for (; symbols[idx] != NULL; idx++) {
    const char *symbol = symbols[idx];
    size_t len = strlen(symbol);
    if (strncmp(symbol, current.offset, len) == 0) {
      current.offset += len;
      current.column += len;
      break;
    }
  }
  if (symbols[idx] == NULL) {
    return NULL;
  }
  token_t *token = allocator_alloc(allocator, sizeof(struct _token_t));
  token->kind = CLUX_TOKEN_SYMBOL;
  token->loc.filename = filename;
  token->loc.begin = begin;
  token->loc.end = current;
  *position = current;
  return token;
}