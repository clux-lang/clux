#ifndef _H_TEST_LEXER_UTIL_
#define _H_TEST_LEXER_UTIL_
#include "core/allocator.h"
#include "core/location.h"
#include "core/position.h"
#include "parser/lexer.h"

/* 构造一个指向 src 起始位置的 position_t。 */
static inline position_t lexer_test_pos(const char *src) {
  position_t p;
  p.offset = src;
  p.line = 1;
  p.column = 1;
  return p;
}

/* 用 location_get 提取 token 的源码文本（调用方负责 allocator_free）。 */
static inline char *lexer_test_text(token_t *tok, allocator_t *a) {
  return location_get(token_get_location(tok), a);
}

#endif
