#include "core/error.h"
#include "engine/interface.h"
#include "engine/value.h"

static clux_value_t *
error_binary(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  (void)ctx;
  return left ? left : right;
}

static clux_value_t *error_prefix(clux_context_t *ctx, clux_value_t *value) {
  (void)ctx;
  return value;
}

static clux_value_t *error_deref_get(clux_context_t *ctx, clux_value_t *value) {
  (void)ctx;
  return value;
}

static clux_value_t *
error_deref_set(clux_context_t *ctx, clux_value_t *ptr, clux_value_t *value) {
  (void)ctx;
  (void)value;
  return ptr;
}

static clux_value_t *
error_safe_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  (void)ctx;
  (void)target;
  return value;
}

static clux_vtable_t g_error_vtable = {
    .add = error_binary,
    .sub = error_binary,
    .mul = error_binary,
    .div = error_binary,
    .mod = error_binary,
    .shl = error_binary,
    .shr = error_binary,
    .bit_and = error_binary,
    .bit_or = error_binary,
    .bit_xor = error_binary,
    .eq = error_binary,
    .ne = error_binary,
    .lt = error_binary,
    .le = error_binary,
    .gt = error_binary,
    .ge = error_binary,
    .plus = error_prefix,
    .negative = error_prefix,
    .bit_not = error_prefix,
    .logic_not = error_prefix,
    .safe_cast = error_safe_cast,
    .cast = error_safe_cast,
    .deref_get = error_deref_get,
    .deref_set = error_deref_set,
};

clux_vtable_t *get_clux_error_vtable(void) { return &g_error_vtable; }
