#ifndef _H_CORE_ERROR_
#define _H_CORE_ERROR_
#include "core/allocator.h"
#include "core/location.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 避免与 glibc <bits/types/error_t.h> 的 `typedef int error_t` 冲突 */
typedef struct _clux_error_t clux_error_t;
clux_error_t *create_error(allocator_t *allocator,
                           const location_t *loc,
                           const char *fmt,
                           ...);
const location_t *error_get_location(clux_error_t *self);
const char *error_get_message(clux_error_t *self);
void delete_error(clux_error_t **error);
#ifdef __cplusplus
}
#endif
#endif