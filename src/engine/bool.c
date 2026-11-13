#include "engine/bool.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static bool bool_read(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(bool *)raw : false;
}

static clux_value_t *
bool_safe_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  clux_type_t *source = clux_value_get_type(value);
  clux_type_kind_t source_kind = clux_type_get_kind(source);
  clux_type_kind_t target_kind = clux_type_get_kind(target);
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  if (source_kind == CLUX_TYPE_ERROR) {
    return value;
  }
  if (target_kind != CLUX_TYPE_BOOL) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot safely cast '%s' to '%s'",
        source_name ? source_name : "<unknown>",
        target_name ? target_name : "<unknown>");
  }
  if (source == target) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  return clux_context_create_bool(ctx, bool_read(value), NULL, NULL);
}

static clux_value_t *
bool_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  clux_type_t *source = clux_value_get_type(value);
  clux_type_kind_t source_kind = clux_type_get_kind(source);
  clux_type_kind_t target_kind = clux_type_get_kind(target);
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  if (source_kind == CLUX_TYPE_ERROR) {
    return value;
  }
  if (source == target) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  bool input = bool_read(value);
  switch (target_kind) {
  case CLUX_TYPE_BOOL:
    return clux_context_create_bool(ctx, input, NULL, NULL);
  case CLUX_TYPE_I8:
    return clux_context_create_i8(ctx, input ? 1 : 0, NULL, NULL);
  case CLUX_TYPE_I16:
    return clux_context_create_i16(ctx, input ? 1 : 0, NULL, NULL);
  case CLUX_TYPE_I32:
    return clux_context_create_i32(ctx, input ? 1 : 0, NULL, NULL);
  case CLUX_TYPE_I64:
    return clux_context_create_i64(ctx, input ? 1 : 0, NULL, NULL);
  case CLUX_TYPE_U8:
    return clux_context_create_u8(ctx, input ? 1 : 0, NULL, NULL);
  case CLUX_TYPE_U16:
    return clux_context_create_u16(ctx, input ? 1 : 0, NULL, NULL);
  case CLUX_TYPE_U32:
    return clux_context_create_u32(ctx, input ? 1 : 0, NULL, NULL);
  case CLUX_TYPE_U64:
    return clux_context_create_u64(ctx, input ? 1 : 0, NULL, NULL);
  case CLUX_TYPE_F32:
    return clux_context_create_f32(ctx, input ? 1.0f : 0.0f, NULL, NULL);
  case CLUX_TYPE_F64:
    return clux_context_create_f64(ctx, input ? 1.0 : 0.0, NULL, NULL);
  default:
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot cast '%s' to '%s'",
        source_name ? source_name : "<unknown>",
        target_name ? target_name : "<unknown>");
  }
}

static clux_value_t *bool_logic_not(clux_context_t *ctx, clux_value_t *value) {
  if (is_error_value(value)) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, type);
  }
  return clux_context_create_bool(ctx, !bool_read(value), NULL, NULL);
}

static clux_value_t *bool_compare(clux_context_t *ctx,
                                  clux_value_t *left,
                                  clux_value_t *right,
                                  clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  clux_type_t *right_type = clux_value_get_type(right);
  if (clux_type_get_kind(right_type) != CLUX_TYPE_BOOL) {
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
  bool left_value = bool_read(left);
  bool right_value = bool_read(right);
  bool result =
      op == CLUX_OP_EQ ? left_value == right_value : left_value != right_value;
  return clux_context_create_bool(ctx, result, NULL, NULL);
}

static clux_value_t *
bool_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return bool_compare(ctx, left, right, CLUX_OP_EQ);
}
static clux_value_t *
bool_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return bool_compare(ctx, left, right, CLUX_OP_NE);
}

static bool bool_type_equal(clux_type_t *self, clux_type_t *other) {
  return clux_type_get_kind(self) == clux_type_get_kind(other);
}

static bool bool_type_extends(clux_type_t *self, clux_type_t *base) {
  return clux_type_get_kind(self) == clux_type_get_kind(base);
}

static clux_vtable_t g_bool_vtable = {
    .logic_not = bool_logic_not,
    .eq = bool_eq,
    .ne = bool_ne,
    .safe_cast = bool_safe_cast,
    .cast = bool_cast,
    .type_equal = bool_type_equal,
    .type_extends = bool_type_extends,
};

clux_vtable_t *get_clux_bool_vtable(void) { return &g_bool_vtable; }
