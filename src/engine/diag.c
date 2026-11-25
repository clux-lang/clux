#include "engine/diag.h"
#include <stdarg.h>

struct _clux_diag_t {
  clux_diag_level_t level;
  clux_error_t *error; /* 本体归 diag 持有 */
  allocator_t *allocator;
};

const char *clux_diag_level_name(clux_diag_level_t level) {
  switch (level) {
  case CLUX_DIAG_ERROR:
    return "error";
  case CLUX_DIAG_WARNING:
    return "warning";
  case CLUX_DIAG_NOTE:
    return "note";
  }
  return "unknown";
}

clux_diag_t *create_clux_diag(allocator_t *allocator,
                              clux_diag_level_t level,
                              const location_t *loc,
                              const char *fmt,
                              ...) {
  va_list args;
  va_start(args, fmt);
  clux_diag_t *diag = create_clux_diagv(allocator, level, loc, fmt, args);
  va_end(args);
  return diag;
}

clux_diag_t *create_clux_diagv(allocator_t *allocator,
                               clux_diag_level_t level,
                               const location_t *loc,
                               const char *fmt,
                               va_list args) {
  if (!allocator) {
    return NULL;
  }
  clux_error_t *error = create_errorv(allocator, loc, fmt, args);
  clux_diag_t *diag = allocator_alloc(allocator, sizeof(struct _clux_diag_t));
  diag->level = level;
  diag->error = error;
  diag->allocator = allocator;
  return diag;
}

void delete_clux_diag(clux_diag_t **diag) {
  if (!diag || !*diag) {
    return;
  }
  delete_error(&(*diag)->error);
  allocator_free((*diag)->allocator, (void **)diag);
}

clux_diag_level_t clux_diag_get_level(const clux_diag_t *self) {
  return self ? self->level : CLUX_DIAG_ERROR;
}

const clux_error_t *clux_diag_get_error(const clux_diag_t *self) {
  return self ? self->error : NULL;
}

string_t *clux_diag_to_string(clux_diag_t *self, allocator_t *allocator) {
  if (!self || !allocator) {
    return NULL;
  }
  return error_to_string_level(
      self->error, allocator, clux_diag_level_name(self->level));
}
