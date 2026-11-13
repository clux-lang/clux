#include "engine/unsigned.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool unsigned_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_U8:
  case CLUX_TYPE_U16:
  case CLUX_TYPE_U32:
  case CLUX_TYPE_U64:
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

static uint64_t uint_read(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return 0;
  }
  switch (clux_type_get_kind(clux_value_get_type(value))) {
  case CLUX_TYPE_U8:
    return *(uint8_t *)raw;
  case CLUX_TYPE_U16:
    return *(uint16_t *)raw;
  case CLUX_TYPE_U32:
    return *(uint32_t *)raw;
  case CLUX_TYPE_U64:
    return *(uint64_t *)raw;
  default:
    return 0;
  }
}

static clux_value_t *
uint_create(clux_context_t *ctx, clux_type_kind_t kind, uint64_t value) {
  switch (kind) {
  case CLUX_TYPE_U8:
    return clux_context_create_u8(ctx, (uint8_t)value, NULL, NULL);
  case CLUX_TYPE_U16:
    return clux_context_create_u16(ctx, (uint16_t)value, NULL, NULL);
  case CLUX_TYPE_U32:
    return clux_context_create_u32(ctx, (uint32_t)value, NULL, NULL);
  case CLUX_TYPE_U64:
    return clux_context_create_u64(ctx, value, NULL, NULL);
  default:
    return NULL;
  }
}

static clux_value_t *
uint_safe_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  clux_type_t *source = clux_value_get_type(value);
  clux_type_kind_t source_kind = clux_type_get_kind(source);
  clux_type_kind_t target_kind = clux_type_get_kind(target);
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  if (source_kind == CLUX_TYPE_ERROR) {
    return value;
  }
  if (!unsigned_kind(target_kind)) {
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
  return uint_create(ctx, target_kind, uint_read(value));
}

static clux_value_t *
uint_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
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
  uint64_t input = uint_read(value);
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
    return clux_context_create_u64(ctx, input, NULL, NULL);
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

static clux_value_t *uint_compare(clux_context_t *ctx,
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
  if (!unsigned_kind(clux_type_get_kind(right_type))) {
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
    left_operand = uint_safe_cast(ctx, left, target);
    if (is_error_value(left_operand)) {
      return left_operand;
    }
  }
  if (right_type != target) {
    right_operand = uint_safe_cast(ctx, right, target);
    if (is_error_value(right_operand)) {
      return right_operand;
    }
  }
  clux_type_t *bool_type = clux_context_get_bool_type(ctx);
  if (clux_value_is_shadow(left_operand) ||
      clux_value_is_shadow(right_operand)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  uint64_t left_value = uint_read(left_operand);
  uint64_t right_value = uint_read(right_operand);
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

static clux_value_t *uint_binary(clux_context_t *ctx,
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
  if (!unsigned_kind(clux_type_get_kind(right_type))) {
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
    left_operand = uint_safe_cast(ctx, left, target);
    if (is_error_value(left_operand)) {
      return left_operand;
    }
  }
  if (right_type != target) {
    right_operand = uint_safe_cast(ctx, right, target);
    if (is_error_value(right_operand)) {
      return right_operand;
    }
  }
  if (clux_value_is_shadow(left_operand) ||
      clux_value_is_shadow(right_operand)) {
    return clux_context_create_shadow_value(ctx, target);
  }

  uint64_t left_value = uint_read(left_operand);
  uint64_t right_value = uint_read(right_operand);
  uint64_t result = 0;
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
    if (right_value == 0) {
      return clux_context_create_error_message(ctx, NULL, "division by zero");
    }
    result = left_value / right_value;
    break;
  case CLUX_OP_MOD:
    if (right_value == 0) {
      return clux_context_create_error_message(ctx, NULL, "division by zero");
    }
    result = left_value % right_value;
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
    if (right_value >= clux_type_get_size(target) * 8) {
      return clux_context_create_error_message(
          ctx, NULL, "shift count out of range");
    }
    if (op == CLUX_OP_SHL) {
      result = left_value << right_value;
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
  return uint_create(ctx, clux_type_get_kind(target), result);
}

static clux_value_t *
uint_prefix(clux_context_t *ctx, clux_value_t *value, clux_prefix_op_t op) {
  if (is_error_value(value)) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, type);
  }
  uint64_t input = uint_read(value);
  uint64_t result = 0;
  switch (op) {
  case CLUX_OP_PLUS:
    result = input;
    break;
  case CLUX_OP_NEGATIVE:
    result = (uint64_t)(0 - input);
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
  return uint_create(ctx, clux_type_get_kind(type), result);
}

static clux_value_t *
uint_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_compare(ctx, left, right, CLUX_OP_EQ);
}
static clux_value_t *
uint_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_compare(ctx, left, right, CLUX_OP_NE);
}
static clux_value_t *
uint_lt(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_compare(ctx, left, right, CLUX_OP_LT);
}
static clux_value_t *
uint_le(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_compare(ctx, left, right, CLUX_OP_LE);
}
static clux_value_t *
uint_gt(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_compare(ctx, left, right, CLUX_OP_GT);
}
static clux_value_t *
uint_ge(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_compare(ctx, left, right, CLUX_OP_GE);
}
static clux_value_t *
uint_add(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_ADD);
}
static clux_value_t *
uint_sub(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_SUB);
}
static clux_value_t *
uint_mul(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_MUL);
}
static clux_value_t *
uint_div(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_DIV);
}
static clux_value_t *
uint_mod(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_MOD);
}
static clux_value_t *
uint_shl(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_SHL);
}
static clux_value_t *
uint_shr(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_SHR);
}
static clux_value_t *
uint_bit_and(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_AND);
}
static clux_value_t *
uint_bit_or(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_OR);
}
static clux_value_t *
uint_bit_xor(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return uint_binary(ctx, left, right, CLUX_OP_XOR);
}
static clux_value_t *uint_plus(clux_context_t *ctx, clux_value_t *value) {
  return uint_prefix(ctx, value, CLUX_OP_PLUS);
}
static clux_value_t *uint_negative(clux_context_t *ctx, clux_value_t *value) {
  return uint_prefix(ctx, value, CLUX_OP_NEGATIVE);
}
static clux_value_t *uint_bit_not(clux_context_t *ctx, clux_value_t *value) {
  return uint_prefix(ctx, value, CLUX_OP_NOT);
}

static clux_vtable_t g_unsigned_vtable = {
    .add = uint_add,
    .sub = uint_sub,
    .mul = uint_mul,
    .div = uint_div,
    .mod = uint_mod,
    .shl = uint_shl,
    .shr = uint_shr,
    .bit_and = uint_bit_and,
    .bit_or = uint_bit_or,
    .bit_xor = uint_bit_xor,
    .eq = uint_eq,
    .ne = uint_ne,
    .lt = uint_lt,
    .le = uint_le,
    .gt = uint_gt,
    .ge = uint_ge,
    .plus = uint_plus,
    .negative = uint_negative,
    .bit_not = uint_bit_not,
    .safe_cast = uint_safe_cast,
    .cast = uint_cast,
};

clux_vtable_t *get_clux_u8_vtable(void) { return &g_unsigned_vtable; }
clux_vtable_t *get_clux_u16_vtable(void) { return &g_unsigned_vtable; }
clux_vtable_t *get_clux_u32_vtable(void) { return &g_unsigned_vtable; }
clux_vtable_t *get_clux_u64_vtable(void) { return &g_unsigned_vtable; }
