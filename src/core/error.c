#include "core/error.h"
#include "core/allocator.h"
#include "core/location.h"
#include <stdarg.h>
#include <stdio.h>
struct _clux_error_t {
  char *msg;
  location_t loc;
  allocator_t *allocator;
};
clux_error_t *create_error(allocator_t *allocator,
                      const location_t *loc,
                      const char *fmt,
                      ...) {
  va_list args;
  va_start(args, fmt);
  size_t len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  char *msg = allocator_alloc(allocator, len + 1);
  va_start(args, fmt);
  (void)vsnprintf(msg, len + 1, fmt, args);
  va_end(args);
  clux_error_t *err = allocator_alloc(allocator, sizeof(struct _clux_error_t));
  err->msg = msg;
  err->loc = *loc;
  err->allocator = allocator;
  return err;
}
const location_t *error_get_location(clux_error_t *self) { return &self->loc; }
const char *error_get_message(clux_error_t *self) { return self->msg; }
void delete_error(clux_error_t **error) {
  if (!error || !*error) {
    return;
  }
  allocator_free((*error)->allocator, (void **)&(*error)->msg);
  allocator_free((*error)->allocator, (void **)error);
}