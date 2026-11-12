#include "engine/str.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool str_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_STR:
    return true;
  default:
    return false;
  }
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static const char *str_read(clux_value_t *value) {
  char **raw = (char **)ptr_to_raw(clux_value_get_data(value), 0);
  return (raw && *raw) ? *raw : "";
}

static clux_value_t *
str_add(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  clux_type_t *left_type = clux_value_get_type(left);
  clux_type_t *right_type = clux_value_get_type(right);
  if (!str_kind(clux_type_get_kind(right_type))) {
    return clux_context_create_error_message(
        ctx, NULL, "operator '+' requires operands of the same type");
  }
  if (clux_value_is_shadow(left) || clux_value_is_shadow(right)) {
    return clux_context_create_shadow_value(ctx, left_type);
  }
  const char *left_str = str_read(left);
  const char *right_str = str_read(right);
  size_t left_len = strlen(left_str);
  size_t right_len = strlen(right_str);
  char *combined = clux_context_alloc_str(ctx, left_len + right_len);
  (void)memcpy(combined, left_str, left_len);
  (void)memcpy(combined + left_len, right_str, right_len);
  return clux_context_create_str_value(ctx, combined, NULL, NULL);
}

static clux_value_t *str_compare(clux_context_t *ctx,
                                 clux_value_t *left,
                                 clux_value_t *right,
                                 clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  clux_type_t *left_type = clux_value_get_type(left);
  clux_type_t *right_type = clux_value_get_type(right);
  if (!str_kind(clux_type_get_kind(right_type))) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "operator '%s' requires operands of the same type",
        op == CLUX_OP_EQ ? "==" : "!=");
  }
  clux_type_t *bool_type = clux_context_get_bool_type(ctx);
  if (clux_value_is_shadow(left) || clux_value_is_shadow(right)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  int result = strcmp(str_read(left), str_read(right));
  bool equal = result == 0;
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
str_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return str_compare(ctx, left, right, CLUX_OP_EQ);
}
static clux_value_t *
str_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return str_compare(ctx, left, right, CLUX_OP_NE);
}

static clux_vtable_t g_str_vtable = {
    .add = str_add,
    .eq = str_eq,
    .ne = str_ne,
};

clux_vtable_t *get_clux_str_vtable(void) { return &g_str_vtable; }
