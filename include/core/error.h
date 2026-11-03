#ifndef _H_CORE_ERROR_
#define _H_CORE_ERROR_
#include "core/allocator.h"
#include "core/location.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _error_t error_t;
error_t *
create_error(allocator_t *allocator, const location_t *loc, const char *fmt, ...);
const location_t *error_get_location(error_t *self);
const char *error_get_message(error_t *self);
void delete_error(error_t **error);
#ifdef __cplusplus
}
#endif
#endif