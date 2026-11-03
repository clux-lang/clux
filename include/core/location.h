#ifndef _H_CORE_LOCATION_
#define _H_CORE_LOCATION_
#include <stdbool.h>
#include <stdint.h>
#include "core/allocator.h"
#include "core/position.h"
#ifdef __cplusplus
extern "C" {
#endif
struct _location_t {
  const char *filename;
  position_t begin;
  position_t end;
};
typedef struct _location_t location_t;
char *location_get(const location_t *loc, allocator_t *allocator);
bool location_is(const location_t *loc, const char *str);
#ifdef __cplusplus
}
#endif
#endif