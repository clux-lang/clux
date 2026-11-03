#include "core/location.h"
#include "core/allocator.h"
char *location_get(const location_t *loc, allocator_t *allocator) {
  size_t len = loc->end.offset - loc->begin.offset;
  char *s = allocator_alloc(allocator, (len + 1) * sizeof(char));
  const char *psrc = loc->begin.offset;
  char *pdst = &s[0];
  while (psrc != loc->end.offset) {
    *pdst++ = *psrc++;
  }
  *pdst = 0;
  return s;
}
bool location_is(const location_t *loc, const char *str) {
  const char *psrc = loc->begin.offset;
  while (psrc != loc->end.offset) {
    if (*psrc != *str) {
      return false;
    }
    psrc++;
    str++;
  }
  if (*str != 0) {
    return false;
  }
  return true;
}