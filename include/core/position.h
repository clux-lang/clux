#ifndef _H_CORE_POSITION_
#define _H_CORE_POSITION_
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct _position_t {
  const char *offset;
  uint32_t column;
  uint32_t line;
};
typedef struct _position_t position_t;
bool position_next(position_t *position);
#ifdef __cplusplus
}
#endif
#endif