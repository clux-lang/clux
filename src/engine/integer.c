#include "engine/integer.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static clux_value_t *integer_safe_cast(clux_context_t *ctx,
                                       clux_value_t *value,
                                       clux_type_t *target);

#define CLUX_DEFINE_INTEGER(suffix, ctype, bits)                               \
  static ctype suffix##_read(clux_value_t *value) {                            \
    void *raw = ptr_to_raw(clux_value_get_data(value), 0);                     \
    return raw ? *(ctype *)raw : 0;                                            \
  }                                                                            \
  static clux_value_t *suffix##_make(clux_context_t *ctx, ctype value) {       \
    return clux_context_create_##suffix(ctx, value, NULL, NULL);               \
  }                                                                            \
  static clux_value_t *suffix##_add(                                           \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    return suffix##_make(ctx,                                                  \
                         (ctype)(suffix##_read(left) + suffix##_read(right))); \
  }                                                                            \
  static clux_value_t *suffix##_sub(                                           \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    return suffix##_make(ctx,                                                  \
                         (ctype)(suffix##_read(left) - suffix##_read(right))); \
  }                                                                            \
  static clux_value_t *suffix##_mul(                                           \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    return suffix##_make(ctx,                                                  \
                         (ctype)(suffix##_read(left) * suffix##_read(right))); \
  }                                                                            \
  static clux_value_t *suffix##_div(                                           \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    ctype divisor = suffix##_read(right);                                      \
    if (divisor == 0) {                                                        \
      return clux_context_create_error_message(ctx, NULL, "division by zero"); \
    }                                                                          \
    return suffix##_make(ctx, (ctype)(suffix##_read(left) / divisor));         \
  }                                                                            \
  static clux_value_t *suffix##_mod(                                           \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    ctype divisor = suffix##_read(right);                                      \
    if (divisor == 0) {                                                        \
      return clux_context_create_error_message(ctx, NULL, "division by zero"); \
    }                                                                          \
    return suffix##_make(ctx, (ctype)(suffix##_read(left) % divisor));         \
  }                                                                            \
  static clux_value_t *suffix##_shl(                                           \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    ctype count = suffix##_read(right);                                        \
    if (count < 0 || (unsigned)count >= (bits)) {                              \
      return clux_context_create_error_message(                                \
          ctx, NULL, "shift count out of range");                              \
    }                                                                          \
    return suffix##_make(                                                      \
        ctx, (ctype)((uint64_t)(ctype)suffix##_read(left) << count));          \
  }                                                                            \
  static clux_value_t *suffix##_shr(                                           \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    ctype count = suffix##_read(right);                                        \
    if (count < 0 || (unsigned)count >= (bits)) {                              \
      return clux_context_create_error_message(                                \
          ctx, NULL, "shift count out of range");                              \
    }                                                                          \
    return suffix##_make(ctx, (ctype)(suffix##_read(left) >> count));          \
  }                                                                            \
  static clux_value_t *suffix##_bit_and(                                       \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    return suffix##_make(ctx,                                                  \
                         (ctype)(suffix##_read(left) & suffix##_read(right))); \
  }                                                                            \
  static clux_value_t *suffix##_bit_or(                                        \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    return suffix##_make(ctx,                                                  \
                         (ctype)(suffix##_read(left) | suffix##_read(right))); \
  }                                                                            \
  static clux_value_t *suffix##_bit_xor(                                       \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    return suffix##_make(ctx,                                                  \
                         (ctype)(suffix##_read(left) ^ suffix##_read(right))); \
  }                                                                            \
  static clux_value_t *suffix##_plus(clux_context_t *ctx, clux_value_t *v) {   \
    return suffix##_make(ctx, suffix##_read(v));                               \
  }                                                                            \
  static clux_value_t *suffix##_negative(clux_context_t *ctx,                  \
                                         clux_value_t *v) {                    \
    return suffix##_make(ctx, (ctype)(-(uint64_t)suffix##_read(v)));           \
  }                                                                            \
  static clux_value_t *suffix##_bit_not(clux_context_t *ctx,                   \
                                        clux_value_t *v) {                     \
    return suffix##_make(ctx, (ctype)(~suffix##_read(v)));                     \
  }                                                                            \
  static clux_vtable_t g_##suffix##_vtable = {                                 \
      .add = suffix##_add,                                                     \
      .sub = suffix##_sub,                                                     \
      .mul = suffix##_mul,                                                     \
      .div = suffix##_div,                                                     \
      .mod = suffix##_mod,                                                     \
      .shl = suffix##_shl,                                                     \
      .shr = suffix##_shr,                                                     \
      .bit_and = suffix##_bit_and,                                             \
      .bit_or = suffix##_bit_or,                                               \
      .bit_xor = suffix##_bit_xor,                                             \
      .plus = suffix##_plus,                                                   \
      .negative = suffix##_negative,                                           \
      .bit_not = suffix##_bit_not,                                             \
      .safe_cast = integer_safe_cast,                                          \
  };                                                                           \
  clux_vtable_t *get_clux_##suffix##_vtable(void) {                            \
    return &g_##suffix##_vtable;                                               \
  }

CLUX_DEFINE_INTEGER(i8, int8_t, 8)
CLUX_DEFINE_INTEGER(i16, int16_t, 16)
CLUX_DEFINE_INTEGER(i32, int32_t, 32)
CLUX_DEFINE_INTEGER(i64, int64_t, 64)

static bool integer_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_I8:
  case CLUX_TYPE_I16:
  case CLUX_TYPE_I32:
  case CLUX_TYPE_I64:
    return true;
  default:
    return false;
  }
}

static int64_t integer_read(clux_value_t *value) {
  switch (clux_type_get_kind(clux_value_get_type(value))) {
  case CLUX_TYPE_I8:
    return i8_read(value);
  case CLUX_TYPE_I16:
    return i16_read(value);
  case CLUX_TYPE_I32:
    return i32_read(value);
  case CLUX_TYPE_I64:
    return i64_read(value);
  default:
    return 0;
  }
}

static clux_value_t *
integer_create(clux_context_t *ctx, clux_type_kind_t kind, int64_t value) {
  switch (kind) {
  case CLUX_TYPE_I8:
    return clux_context_create_i8(ctx, (int8_t)value, NULL, NULL);
  case CLUX_TYPE_I16:
    return clux_context_create_i16(ctx, (int16_t)value, NULL, NULL);
  case CLUX_TYPE_I32:
    return clux_context_create_i32(ctx, (int32_t)value, NULL, NULL);
  case CLUX_TYPE_I64:
    return clux_context_create_i64(ctx, value, NULL, NULL);
  default:
    return NULL;
  }
}

static clux_value_t *integer_safe_cast(clux_context_t *ctx,
                                       clux_value_t *value,
                                       clux_type_t *target) {
  clux_type_t *source = clux_value_get_type(value);
  clux_type_kind_t target_kind = clux_type_get_kind(target);
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  if (!integer_kind(target_kind)) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot safely cast '%s' to '%s'",
        source_name ? source_name : "<unknown>",
        target_name ? target_name : "<unknown>");
  }
  if (clux_type_get_size(target) < clux_type_get_size(source)) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot safely cast '%s' to '%s': narrowing conversion",
        source_name ? source_name : "<unknown>",
        target_name ? target_name : "<unknown>");
  }
  return integer_create(ctx, target_kind, integer_read(value));
}
