#include "engine/integer.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

static bool is_error_value(clux_context_t *ctx, clux_value_t *value) {
  (void)ctx;
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static int64_t integer_read(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return 0;
  }
  switch (clux_type_get_kind(clux_value_get_type(value))) {
  case CLUX_TYPE_I8:
    return *(int8_t *)raw;
  case CLUX_TYPE_I16:
    return *(int16_t *)raw;
  case CLUX_TYPE_I32:
    return *(int32_t *)raw;
  case CLUX_TYPE_I64:
    return *(int64_t *)raw;
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
  clux_type_kind_t source_kind = clux_type_get_kind(source);
  clux_type_kind_t target_kind = clux_type_get_kind(target);
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  if (source_kind == CLUX_TYPE_ERROR) {
    return value;
  }
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
  if (source == target) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  return integer_create(ctx, target_kind, integer_read(value));
}

static clux_value_t *
integer_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
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
  int64_t input = integer_read(value);
  switch (target_kind) {
  case CLUX_TYPE_I8:
    return clux_context_create_i8(ctx, (int8_t)input, NULL, NULL);
  case CLUX_TYPE_I16:
    return clux_context_create_i16(ctx, (int16_t)input, NULL, NULL);
  case CLUX_TYPE_I32:
    return clux_context_create_i32(ctx, (int32_t)input, NULL, NULL);
  case CLUX_TYPE_I64:
    return clux_context_create_i64(ctx, input, NULL, NULL);
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
    return clux_context_create_f64(ctx, (double)input, NULL, NULL);
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

static clux_value_t *integer_compare(clux_context_t *ctx,
                                     clux_value_t *left,
                                     clux_value_t *right,
                                     clux_binary_op_t op) {
  if (is_error_value(ctx, left)) {
    return left;
  }
  if (is_error_value(ctx, right)) {
    return right;
  }
  clux_type_t *left_type = clux_value_get_type(left);
  clux_type_t *right_type = clux_value_get_type(right);
  if (!integer_kind(clux_type_get_kind(right_type))) {
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
    left_operand = integer_safe_cast(ctx, left, target);
    if (is_error_value(ctx, left_operand)) {
      return left_operand;
    }
  }
  if (right_type != target) {
    right_operand = integer_safe_cast(ctx, right, target);
    if (is_error_value(ctx, right_operand)) {
      return right_operand;
    }
  }
  clux_type_t *bool_type = clux_context_get_bool_type(ctx);
  if (clux_value_is_shadow(left_operand) ||
      clux_value_is_shadow(right_operand)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  int64_t left_value = integer_read(left_operand);
  int64_t right_value = integer_read(right_operand);
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

static clux_value_t *integer_binary(clux_context_t *ctx,
                                    clux_value_t *left,
                                    clux_value_t *right,
                                    clux_binary_op_t op) {
  if (is_error_value(ctx, left)) {
    return left;
  }
  if (is_error_value(ctx, right)) {
    return right;
  }
  clux_type_t *left_type = clux_value_get_type(left);
  clux_type_t *right_type = clux_value_get_type(right);
  if (!integer_kind(clux_type_get_kind(right_type))) {
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
    left_operand = integer_safe_cast(ctx, left, target);
    if (is_error_value(ctx, left_operand)) {
      return left_operand;
    }
  }
  if (right_type != target) {
    right_operand = integer_safe_cast(ctx, right, target);
    if (is_error_value(ctx, right_operand)) {
      return right_operand;
    }
  }
  if (clux_value_is_shadow(left_operand) ||
      clux_value_is_shadow(right_operand)) {
    return clux_context_create_shadow_value(ctx, target);
  }

  int64_t left_value = integer_read(left_operand);
  int64_t right_value = integer_read(right_operand);
  int64_t result = 0;
  switch (op) {
  case CLUX_OP_ADD:
    result = (int64_t)((uint64_t)left_value + (uint64_t)right_value);
    break;
  case CLUX_OP_SUB:
    result = (int64_t)((uint64_t)left_value - (uint64_t)right_value);
    break;
  case CLUX_OP_MUL:
    result = (int64_t)((uint64_t)left_value * (uint64_t)right_value);
    break;
  case CLUX_OP_DIV:
    if (right_value == 0) {
      return clux_context_create_error_message(ctx, NULL, "division by zero");
    }
    if (left_value == INT64_MIN && right_value == -1) {
      result = INT64_MIN;
    } else {
      result = left_value / right_value;
    }
    break;
  case CLUX_OP_MOD:
    if (right_value == 0) {
      return clux_context_create_error_message(ctx, NULL, "division by zero");
    }
    if (left_value == INT64_MIN && right_value == -1) {
      result = 0;
    } else {
      result = left_value % right_value;
    }
    break;
  case CLUX_OP_AND:
    result = left_value & right_value;
    break;
  case CLUX_OP_OR:
    result = left_value | right_value;
    break;
  case CLUX_OP_XOR:
    result = left_value ^ right_value;
    break;
  case CLUX_OP_SHL:
  case CLUX_OP_SHR:
    if (right_value < 0 ||
        (uint64_t)right_value >= clux_type_get_size(target) * 8) {
      return clux_context_create_error_message(
          ctx, NULL, "shift count out of range");
    }
    if (op == CLUX_OP_SHL) {
      result = (int64_t)((uint64_t)left_value << right_value);
    } else {
      result = left_value >> right_value;
    }
    break;
  default:
    return clux_context_create_error_message(
        ctx,
        NULL,
        "type '%s' does not support operator '%s'",
        clux_type_get_name(target),
        binary_op_symbol(op));
  }
  return integer_create(ctx, clux_type_get_kind(target), result);
}

static clux_value_t *
integer_prefix(clux_context_t *ctx, clux_value_t *value, clux_prefix_op_t op) {
  if (is_error_value(ctx, value)) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, type);
  }
  int64_t input = integer_read(value);
  int64_t result = 0;
  switch (op) {
  case CLUX_OP_PLUS:
    result = input;
    break;
  case CLUX_OP_NEGATIVE:
    result = (int64_t)(0 - (uint64_t)input);
    break;
  case CLUX_OP_NOT:
    result = ~input;
    break;
  case CLUX_OP_LNOT:
    return clux_context_create_error_message(
        ctx,
        NULL,
        "type '%s' does not support operator '!'",
        clux_type_get_name(type));
  }
  return integer_create(ctx, clux_type_get_kind(type), result);
}

static clux_value_t *
integer_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_compare(ctx, left, right, CLUX_OP_EQ);
}
static clux_value_t *
integer_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_compare(ctx, left, right, CLUX_OP_NE);
}
static clux_value_t *
integer_lt(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_compare(ctx, left, right, CLUX_OP_LT);
}
static clux_value_t *
integer_le(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_compare(ctx, left, right, CLUX_OP_LE);
}
static clux_value_t *
integer_gt(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_compare(ctx, left, right, CLUX_OP_GT);
}
static clux_value_t *
integer_ge(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_compare(ctx, left, right, CLUX_OP_GE);
}
static clux_value_t *
integer_add(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_ADD);
}
static clux_value_t *
integer_sub(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_SUB);
}
static clux_value_t *
integer_mul(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_MUL);
}
static clux_value_t *
integer_div(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_DIV);
}
static clux_value_t *
integer_mod(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_MOD);
}
static clux_value_t *
integer_shl(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_SHL);
}
static clux_value_t *
integer_shr(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_SHR);
}
static clux_value_t *
integer_bit_and(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_AND);
}
static clux_value_t *
integer_bit_or(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_OR);
}
static clux_value_t *
integer_bit_xor(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return integer_binary(ctx, left, right, CLUX_OP_XOR);
}
static clux_value_t *integer_plus(clux_context_t *ctx, clux_value_t *value) {
  return integer_prefix(ctx, value, CLUX_OP_PLUS);
}
static clux_value_t *integer_negative(clux_context_t *ctx,
                                      clux_value_t *value) {
  return integer_prefix(ctx, value, CLUX_OP_NEGATIVE);
}
static clux_value_t *integer_bit_not(clux_context_t *ctx, clux_value_t *value) {
  return integer_prefix(ctx, value, CLUX_OP_NOT);
}

static clux_vtable_t g_integer_vtable = {
    .add = integer_add,
    .sub = integer_sub,
    .mul = integer_mul,
    .div = integer_div,
    .mod = integer_mod,
    .shl = integer_shl,
    .shr = integer_shr,
    .bit_and = integer_bit_and,
    .bit_or = integer_bit_or,
    .bit_xor = integer_bit_xor,
    .eq = integer_eq,
    .ne = integer_ne,
    .lt = integer_lt,
    .le = integer_le,
    .gt = integer_gt,
    .ge = integer_ge,
    .plus = integer_plus,
    .negative = integer_negative,
    .bit_not = integer_bit_not,
    .safe_cast = integer_safe_cast,
    .cast = integer_cast,
};

clux_vtable_t *get_clux_i8_vtable(void) { return &g_integer_vtable; }
clux_vtable_t *get_clux_i16_vtable(void) { return &g_integer_vtable; }
clux_vtable_t *get_clux_i32_vtable(void) { return &g_integer_vtable; }
clux_vtable_t *get_clux_i64_vtable(void) { return &g_integer_vtable; }
