#include "core/position.h"
#include <unicode/utf8.h>

bool position_next(position_t *position) {
  if (*position->offset == '\0') {
    return false;
  }
  const uint8_t *s = (const uint8_t *)position->offset;
  int32_t i = 0;
  UChar32 c;
  /* length=-1: NUL-terminated, safe macro handles boundary */
  U8_NEXT(s, i, -1, c);
  if (c < 0) {
    /* illegal UTF-8 sequence: advance one byte as fallback */
    i = 1;
  }
  if (c == (UChar32)'\n') {
    position->line++;
    position->column = 1;
  } else {
    position->column++;
  }
  position->offset += i;
  return true;
}