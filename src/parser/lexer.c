#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/position.h"
#include <unicode/utf8.h>

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
