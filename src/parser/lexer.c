#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/location.h"
#include "core/position.h"
#include <string.h>
#include <unicode/utf8.h>
#include <unicode/uchar.h>

struct _token_t {
  token_kind_t kind;
  location_t loc;
  allocator_t *allocator;
};
/* 从 position 读取一个 UTF-8 码点但不推进 position，返回码点或 -1（EOF/非法）。 */
static UChar32 peek_codepoint(const position_t *pos) {
  if (*pos->offset == '\0') return -1;
  const uint8_t *s = (const uint8_t *)pos->offset;
  int32_t i = 0;
  UChar32 c;
  U8_NEXT(s, i, -1, c);
  return c;
}

/* 扫描到闭合引号（含），成功返回 true 并推进 current 越过闭合引号。
   失败返回 false，current 停在失败处。失败条件仅两种：
     - 遇到非法 UTF-8 码点
     - 未闭合（EOF 前没找到闭合引号）
   转义序列只做"跳过被转义字符"处理，不做语义校验——无效转义、
   空/多码点字符等内容错误留给 parser 报。 */
static bool scan_quoted(position_t *current, char quote) {
  while (1) {
    if (*current->offset == '\0') return false; /* 未闭合 */
    UChar32 c = peek_codepoint(current);
    if (c < 0) return false; /* 非法 UTF-8 */
    if (c == quote) {
      position_next(current); /* 闭合引号 */
      return true;
    }
    if (c == '\\') {
      position_next(current); /* 反斜杠 */
      if (*current->offset == '\0') return false;
      UChar32 escaped = peek_codepoint(current);
      if (escaped < 0) return false;
      position_next(current); /* 被转义字符，内容不校验 */
      continue;
    }
    position_next(current);
  }
}

token_t *
create_token(allocator_t *allocator, token_kind_t kind, location_t loc) {
  token_t *self = allocator_alloc(allocator, sizeof(struct _token_t));
  self->kind = kind;
  self->loc = loc;
  self->allocator = allocator;
  return self;
}
void delete_token(token_t **token) {
  if (!token || !*token) {
    return;
  }
  token_t *self = *token;
  allocator_free(self->allocator, (void **)token);
}
token_kind_t token_get_kind(token_t *self) { return self->kind; }
const location_t *token_get_location(token_t *self) { return &self->loc; }

token_t *lexer_read_character(allocator_t *allocator,
                              position_t *position,
                              const char *filename) {
  if (*position->offset != '\'') {
    return NULL;
  }
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* 越过开头的 '\'' */

  /* 只检测非法 UTF-8 与缺失闭合引号；空字符、多字符、无效转义
     等内容问题留给 parser 报。 */
  token_kind_t kind =
      scan_quoted(&current, '\'') ? CLUX_TOKEN_CHARACTER : CLUX_TOKEN_ERROR;

  location_t loc = {
      .filename = filename,
      .begin = begin,
      .end = current,
  };
  token_t *token = create_token(allocator, kind, loc);
  *position = current;
  return token;
}

/* ---- string ----
   "..." — 空串 "" 合法。只检测非法 UTF-8 与缺失闭合引号；
   无效转义等内容问题留给 parser 报。 */
token_t *lexer_read_string(allocator_t *allocator,
                           position_t *position,
                           const char *filename) {
  if (*position->offset != '"') return NULL;
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* 越过开头的 '"' */

  token_kind_t kind =
      scan_quoted(&current, '"') ? CLUX_TOKEN_STRING : CLUX_TOKEN_ERROR;

  location_t loc = {
      .filename = filename,
      .begin = begin,
      .end = current,
  };
  token_t *token = create_token(allocator, kind, loc);
  *position = current;
  return token;
}

static const char *keywords[] = {
    "var",      "func",      "cfunc",   "import",     "export",   "if",
    "else",     "for",       "foreach", "while",      "do",       "break",
    "continue", "return",    "using",   "const",      "volatile", "bool",
    "i8",       "i16",       "i32",     "i64",        "u8",       "u16",
    "u32",      "u64",       "isize",   "usize",      "f32",      "f64",
    "str",      "void",      "opaque",  "struct",     "union",    "cunion",
    "enum",     "undefined", "none",    "nil",        "true",     "false",
    "as",       "is",        "extends", "implements", "comptime", "type",
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
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* 消费首字符 */

  while (1) {
    c = peek_codepoint(&current);
    if (c < 0 || !u_isIDPart(c)) break;
    position_next(&current);
  }
  location_t loc = {
      .filename = filename,
      .begin = begin,
      .end = current,
  };
  token_kind_t kind = CLUX_TOKEN_IDENTIFIER;
  for (size_t idx = 0; keywords[idx] != NULL; idx++) {
    if (location_is(&loc, keywords[idx])) {
      kind = CLUX_TOKEN_KEYWORD;
      break;
    }
  }
  token_t *token = create_token(allocator, kind, loc);
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
  position_t begin = *position;
  position_t current = *position;

  while (1) {
    c = peek_codepoint(&current);
    if (c < 0 || !u_isWhitespace(c)) break;
    position_next(&current);
  }

  location_t loc = {
      .filename = filename,
      .begin = begin,
      .end = current,
  };
  token_t *token = create_token(allocator, CLUX_TOKEN_WHITESPACE, loc);
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
  position_t begin = *position;
  position_t current = *position;
  position_next(&current); /* 第一个 '/' */
  position_next(&current); /* 第二个 '/' */

  while (*current.offset != '\0' && *current.offset != '\n') {
    position_next(&current);
  }
  /* 不消费换行，留给下一个 token */

  location_t loc = {
      .filename = filename,
      .begin = begin,
      .end = current,
  };
  token_t *token = create_token(allocator, CLUX_TOKEN_COMMENT, loc);
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
  position_t begin = *position;
  position_t current = *position;
  token_kind_t kind = CLUX_TOKEN_MULTILINE_COMMENT;
  position_next(&current); /* '/' */
  position_next(&current); /* '*' */

  while (1) {
    if (*current.offset == '\0') {
      kind = CLUX_TOKEN_ERROR; /* 未闭合 */
      break;
    }
    if (current.offset[0] == '*' && current.offset[1] == '/') {
      position_next(&current); /* '*' */
      position_next(&current); /* '/' */
      break;
    }
    position_next(&current);
  }

  location_t loc = {
      .filename = filename,
      .begin = begin,
      .end = current,
  };
  token_t *token = create_token(allocator, kind, loc);
  *position = current;
  return token;
}

/* ---- numeric ----
   数字字面量：
     十六进制: 0x[0-9a-fA-F]*
     八进制:   0[0-7]*
     十进制:   [0-9]+
     浮点:     [0-9]+.[0-9]+([eE][+-]?[0-9]+)?
   clux 禁止浮点的两种简写：`.123`（省略整数部分）和 `123.`（省略小数部分）。
   因此点号只有在其后紧跟数字时才属于数字字面量，否则留给后续 token——
   `123.!` 切分为 `123` + `.!`，`a.123` 切分为 `a` + `.` + `123`。
   非数字开头返回 NULL。内容合法性（如 0x 后无数字）由 parser 校验。 */
token_t *lexer_read_numeric(allocator_t *allocator,
                            position_t *position,
                            const char *filename) {
  char c0 = position->offset[0];
  if (!(c0 >= '0' && c0 <= '9')) return NULL;

  position_t begin = *position;
  position_t current = *position;

  if (c0 == '0' && (position->offset[1] == 'x' || position->offset[1] == 'X')) {
    /* 十六进制。0x 后无数字属于内容错误，留给 parser 报。 */
    position_next(&current); /* '0' */
    position_next(&current); /* 'x'/'X' */
    while (1) {
      char h = *current.offset;
      if ((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
          (h >= 'A' && h <= 'F')) {
        position_next(&current);
      } else {
        break;
      }
    }
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
    /* 小数部分：点号后必须紧跟数字，否则不属于本字面量（禁止 `123.` 简写）。 */
    if (*current.offset == '.' && current.offset[1] >= '0' &&
        current.offset[1] <= '9') {
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

  location_t loc = {
      .filename = filename,
      .begin = begin,
      .end = current,
  };
  token_t *token = create_token(allocator, CLUX_TOKEN_NUMERIC, loc);
  *position = current;
  return token;
}

/* 注意：`<<` / `>>` / `.?` / `||` / `[[` / `]]` 故意不在此表中。它们在
   词法层有歧义（`<<` / `>>` 与嵌套元组的尖括号冲突，`.?` 与 optional 构造
   冲突，`||` 与空捕获列表冲突，`[[` / `]]` 与嵌套下标冲突），
   由 parser 在相邻字符位置按上下文组合。 */
static const char *symbols[] = {
    "...", "||=", "&&=", ">>=", "<<=", "==", "!=", ">=", "<=", "&&",
    ".!",  ".*",  ".&",  "+=",  "-=",  "/=", "*=", "%=", "&=", "|=",
    "~=",  "^=",  "->",  "::",  "+",   "-",  "*",  "/",  "%",  "&",
    "|",   "!",   "=",   "(",   ")",   "[",  "]",  "{",  "}",  "~",
    "^",   ".",   ",",   "?",   ":",   ";",  ">",  "<",  NULL};

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
  location_t loc = {
      .filename = filename,
      .begin = begin,
      .end = current,
  };
  token_t *token = create_token(allocator, CLUX_TOKEN_SYMBOL, loc);
  *position = current;
  return token;
}

bool token_is(token_t *self, token_kind_t kind, const char *src) {
  return self && self->kind == kind && location_is(&self->loc, src);
}