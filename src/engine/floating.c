#include "engine/floating.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool floating_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_F32:
  case CLUX_TYPE_F64:
    return true;
  default:
    return false;
  }
}

static bool numeric_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_I8:
  case CLUX_TYPE_I16:
  case CLUX_TYPE_I32:
  case CLUX_TYPE_I64:
  case CLUX_TYPE_U8:
  case CLUX_TYPE_U16:
  case CLUX_TYPE_U32:
  case CLUX_TYPE_U64:
  case CLUX_TYPE_F32:
  case CLUX_TYPE_F64:
  case CLUX_TYPE_BOOL:
    return true;
  default:
    return false;
  }
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static double float_read(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return 0;
  }
  switch (clux_type_get_kind(clux_value_get_type(value))) {
  case CLUX_TYPE_F32:
    return *(float *)raw;
  case CLUX_TYPE_F64:
    return *(double *)raw;
  default:
    return 0;
  }
}

static clux_value_t *
float_create(clux_context_t *ctx, clux_type_kind_t kind, double value) {
  switch (kind) {
  case CLUX_TYPE_F32:
    return clux_context_create_f32(ctx, (float)value, NULL, NULL);
  case CLUX_TYPE_F64:
    return clux_context_create_f64(ctx, value, NULL, NULL);
  default:
    return NULL;
  }
}

static clux_value_t *
float_safe_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  clux_type_t *source = clux_value_get_type(value);
  clux_type_kind_t source_kind = clux_type_get_kind(source);
  clux_type_kind_t target_kind = clux_type_get_kind(target);
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  if (source_kind == CLUX_TYPE_ERROR) {
    return value;
  }
  if (!floating_kind(target_kind)) {
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
  if (source == target) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  return float_create(ctx, target_kind, float_read(value));
}

static clux_value_t *
float_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  clux_type_t *source = clux_value_get_type(value);
  clux_type_kind_t source_kind = clux_type_get_kind(source);
  clux_type_kind_t target_kind = clux_type_get_kind(target);
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  if (source_kind == CLUX_TYPE_ERROR) {
    return value;
  }
  if (!numeric_kind(target_kind)) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot cast '%s' to '%s'",
        source_name ? source_name : "<unknown>",
        target_name ? target_name : "<unknown>");
  }
  if (source == target) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  double input = float_read(value);
  switch (target_kind) {
  case CLUX_TYPE_I8:
    return clux_context_create_i8(ctx, (int8_t)input, NULL, NULL);
  case CLUX_TYPE_I16:
    return clux_context_create_i16(ctx, (int16_t)input, NULL, NULL);
  case CLUX_TYPE_I32:
    return clux_context_create_i32(ctx, (int32_t)input, NULL, NULL);
  case CLUX_TYPE_I64:
    return clux_context_create_i64(ctx, (int64_t)input, NULL, NULL);
  case CLUX_TYPE_U8:
    return clux_context_create_u8(ctx, (uint8_t)input, NULL, NULL);
  case CLUX_TYPE_U16:
    return clux_context_create_u16(ctx, (uint16_t)input, NULL, NULL);
  case CLUX_TYPE_U32:
    return clux_context_create_u32(ctx, (uint32_t)input, NULL, NULL);
  case CLUX_TYPE_U64:
    return clux_context_create_u64(ctx, (uint64_t)input, NULL, NULL);
  case CLUX_TYPE_F32:
    return clux_context_create_f32(ctx, (float)input, NULL, NULL);
  case CLUX_TYPE_F64:
    return clux_context_create_f64(ctx, input, NULL, NULL);
  case CLUX_TYPE_BOOL:
    return clux_context_create_bool(ctx, input != 0, NULL, NULL);
  default:
    return NULL;
  }
}

static const char *binary_op_symbol(clux_binary_op_t op) {
  switch (op) {
  case CLUX_OP_ADD:
    return "+";
  case CLUX_OP_SUB:
    return "-";
  case CLUX_OP_MUL:
    return "*";
  case CLUX_OP_DIV:
    return "/";
  case CLUX_OP_MOD:
    return "%";
  case CLUX_OP_AND:
    return "&";
  case CLUX_OP_OR:
    return "|";
  case CLUX_OP_XOR:
    return "^";
  case CLUX_OP_SHL:
    return "<<";
  case CLUX_OP_SHR:
    return ">>";
  case CLUX_OP_EQ:
    return "==";
  case CLUX_OP_NE:
    return "!=";
  case CLUX_OP_LT:
    return "<";
  case CLUX_OP_LE:
    return "<=";
  case CLUX_OP_GT:
    return ">";
  case CLUX_OP_GE:
    return ">=";
  }
  return "<unknown>";
}

static clux_value_t *float_compare(clux_context_t *ctx,
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
  if (!floating_kind(clux_type_get_kind(right_type))) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "operator '%s' requires operands of the same type",
        binary_op_symbol(op));
  }
  clux_type_t *target =
      clux_type_get_size(left_type) >= clux_type_get_size(right_type)
          ? left_type
          : right_type;
  clux_value_t *left_operand = left;
  clux_value_t *right_operand = right;
  if (left_type != target) {
    left_operand = float_safe_cast(ctx, left, target);
    if (is_error_value(left_operand)) {
      return left_operand;
    }
  }
  if (right_type != target) {
    right_operand = float_safe_cast(ctx, right, target);
    if (is_error_value(right_operand)) {
      return right_operand;
    }
  }
  clux_type_t *bool_type = clux_context_get_bool_type(ctx);
  if (clux_value_is_shadow(left_operand) ||
      clux_value_is_shadow(right_operand)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  double left_value = float_read(left_operand);
  double right_value = float_read(right_operand);
  bool result = false;
  switch (op) {
  case CLUX_OP_EQ:
    result = left_value == right_value;
    break;
  case CLUX_OP_NE:
    result = left_value != right_value;
    break;
  case CLUX_OP_LT:
    result = left_value < right_value;
    break;
  case CLUX_OP_LE:
    result = left_value <= right_value;
    break;
  case CLUX_OP_GT:
    result = left_value > right_value;
    break;
  case CLUX_OP_GE:
    result = left_value >= right_value;
    break;
  default:
    return NULL;
  }
  return clux_context_create_bool(ctx, result, NULL, NULL);
}

static clux_value_t *float_binary(clux_context_t *ctx,
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
  if (!floating_kind(clux_type_get_kind(right_type))) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "operator '%s' requires operands of the same type",
        binary_op_symbol(op));
  }
  clux_type_t *target =
      clux_type_get_size(left_type) >= clux_type_get_size(right_type)
          ? left_type
          : right_type;
  clux_value_t *left_operand = left;
  clux_value_t *right_operand = right;
  if (left_type != target) {
    left_operand = float_safe_cast(ctx, left, target);
    if (is_error_value(left_operand)) {
      return left_operand;
    }
  }
  if (right_type != target) {
    right_operand = float_safe_cast(ctx, right, target);
    if (is_error_value(right_operand)) {
      return right_operand;
    }
  }
  if (clux_value_is_shadow(left_operand) ||
      clux_value_is_shadow(right_operand)) {
    return clux_context_create_shadow_value(ctx, target);
  }

  double left_value = float_read(left_operand);
  double right_value = float_read(right_operand);
  double result = 0;
  switch (op) {
  case CLUX_OP_ADD:
    result = left_value + right_value;
    break;
  case CLUX_OP_SUB:
    result = left_value - right_value;
    break;
  case CLUX_OP_MUL:
    result = left_value * right_value;
    break;
  case CLUX_OP_DIV:
    result = left_value / right_value;
    break;
  case CLUX_OP_MOD:
  case CLUX_OP_AND:
  case CLUX_OP_OR:
  case CLUX_OP_XOR:
  case CLUX_OP_SHL:
  case CLUX_OP_SHR:
  default:
    return clux_context_create_error_message(
        ctx,
        NULL,
        "type '%s' does not support operator '%s'",
        clux_type_get_name(target),
        binary_op_symbol(op));
  }
  return float_create(ctx, clux_type_get_kind(target), result);
}

static clux_value_t *
float_prefix(clux_context_t *ctx, clux_value_t *value, clux_prefix_op_t op) {
  if (is_error_value(value)) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, type);
  }
  double input = float_read(value);
  double result = 0;
  switch (op) {
  case CLUX_OP_PLUS:
    result = input;
    break;
  case CLUX_OP_NEGATIVE:
    result = -input;
    break;
  case CLUX_OP_NOT:
  case CLUX_OP_LNOT:
    return clux_context_create_error_message(
        ctx,
        NULL,
        "type '%s' does not support operator '%s'",
        clux_type_get_name(type),
        op == CLUX_OP_NOT ? "~" : "!");
  }
  return float_create(ctx, clux_type_get_kind(type), result);
}

static clux_value_t *
float_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_compare(ctx, left, right, CLUX_OP_EQ);
}
static clux_value_t *
float_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_compare(ctx, left, right, CLUX_OP_NE);
}
static clux_value_t *
float_lt(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_compare(ctx, left, right, CLUX_OP_LT);
}
static clux_value_t *
float_le(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_compare(ctx, left, right, CLUX_OP_LE);
}
static clux_value_t *
float_gt(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_compare(ctx, left, right, CLUX_OP_GT);
}
static clux_value_t *
float_ge(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_compare(ctx, left, right, CLUX_OP_GE);
}
static clux_value_t *
float_add(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_binary(ctx, left, right, CLUX_OP_ADD);
}
static clux_value_t *
float_sub(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_binary(ctx, left, right, CLUX_OP_SUB);
}
static clux_value_t *
float_mul(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_binary(ctx, left, right, CLUX_OP_MUL);
}
static clux_value_t *
float_div(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return float_binary(ctx, left, right, CLUX_OP_DIV);
}
static clux_value_t *float_plus(clux_context_t *ctx, clux_value_t *value) {
  return float_prefix(ctx, value, CLUX_OP_PLUS);
}
static clux_value_t *float_negative(clux_context_t *ctx, clux_value_t *value) {
  return float_prefix(ctx, value, CLUX_OP_NEGATIVE);
}

static bool floating_type_equal(clux_type_t *self, clux_type_t *other) {
  return clux_type_get_kind(self) == clux_type_get_kind(other);
}

static bool floating_type_extends(clux_type_t *self, clux_type_t *base) {
  return clux_type_get_kind(self) == clux_type_get_kind(base);
}

static clux_vtable_t g_floating_vtable = {
    .add = float_add,
    .sub = float_sub,
    .mul = float_mul,
    .div = float_div,
    .plus = float_plus,
    .negative = float_negative,
    .eq = float_eq,
    .ne = float_ne,
    .lt = float_lt,
    .le = float_le,
    .gt = float_gt,
    .ge = float_ge,
    .safe_cast = float_safe_cast,
    .cast = float_cast,
    .type_equal = floating_type_equal,
    .type_extends = floating_type_extends,
};

clux_vtable_t *get_clux_f32_vtable(void) { return &g_floating_vtable; }
clux_vtable_t *get_clux_f64_vtable(void) { return &g_floating_vtable; }
