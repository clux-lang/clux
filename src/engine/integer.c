#include "engine/integer.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stddef.h>
#include <stdint.h>

#define CLUX_DEFINE_INTEGER(suffix, ctype)                                     \
  static ctype suffix##_read(clux_value_t *value) {                            \
    void *raw = ptr_to_raw(clux_value_get_data(value), 0);                     \
    return raw ? *(ctype *)raw : 0;                                            \
  }                                                                            \
  static clux_value_t *suffix##_add(clux_context_t *ctx,                       \
                                    clux_value_t *left,                        \
                                    clux_binary_op_t op,                       \
                                    clux_value_t *right) {                     \
    (void)op;                                                                  \
    ctype result = (ctype)(suffix##_read(left) + suffix##_read(right));        \
    return clux_context_create_##suffix(ctx, result, NULL, NULL);              \
  }                                                                            \
  static clux_vtable_t g_##suffix##_vtable = {                                 \
      .add = suffix##_add,                                                     \
  };                                                                           \
  clux_vtable_t *get_clux_##suffix##_vtable(void) {                            \
    return &g_##suffix##_vtable;                                               \
  }

CLUX_DEFINE_INTEGER(i8, int8_t)
CLUX_DEFINE_INTEGER(i16, int16_t)
CLUX_DEFINE_INTEGER(i32, int32_t)
CLUX_DEFINE_INTEGER(i64, int64_t)
