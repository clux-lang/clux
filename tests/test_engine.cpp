#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t g_alloc_count;
static size_t g_free_count;

static void *counting_alloc(size_t size) {
  g_alloc_count++;
  return malloc(size);
}

static void counting_free(void *ptr) {
  g_free_count++;
  free(ptr);
}

static int8_t read_i8(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int8_t *)raw : 0;
}

static int16_t read_i16(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int16_t *)raw : 0;
}

static int32_t read_i32(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int32_t *)raw : 0;
}

static int64_t read_i64(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int64_t *)raw : 0;
}

static const char *error_message_of(clux_value_t *value) {
  clux_error_t **stored =
      (clux_error_t **)ptr_to_raw(clux_value_get_data(value), 0);
  return stored ? error_get_message(*stored) : nullptr;
}

TEST(EngineTest, ContextAndBuiltinTypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  ASSERT_NE(ctx, nullptr);

  clux_type_t *i32_type = clux_context_get_i32_type(ctx);
  ASSERT_NE(i32_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(i32_type), CLUX_TYPE_I32);
  EXPECT_STREQ(clux_type_get_name(i32_type), "i32");
  EXPECT_EQ(clux_type_get_size(i32_type), sizeof(int32_t));
  EXPECT_EQ(clux_type_get_align(i32_type), alignof(int32_t));
  EXPECT_NE(clux_type_get_vtable(i32_type), nullptr);

  clux_type_t *error_type = clux_context_get_error_type(ctx);
  ASSERT_NE(error_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(error_type), CLUX_TYPE_ERROR);
  EXPECT_STREQ(clux_type_get_name(error_type), "error");

  delete_clux_context(&ctx);
  EXPECT_EQ(ctx, nullptr);
  delete_allocator(&a);
}

TEST(EngineTest, BuiltinIntegerTypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  struct {
    clux_type_t *type;
    clux_type_kind_t kind;
    const char *name;
    size_t size;
  } cases[] = {
      {clux_context_get_i8_type(ctx), CLUX_TYPE_I8, "i8", sizeof(int8_t)},
      {clux_context_get_i16_type(ctx), CLUX_TYPE_I16, "i16", sizeof(int16_t)},
      {clux_context_get_i32_type(ctx), CLUX_TYPE_I32, "i32", sizeof(int32_t)},
      {clux_context_get_i64_type(ctx), CLUX_TYPE_I64, "i64", sizeof(int64_t)},
  };
  for (const auto &c : cases) {
    ASSERT_NE(c.type, nullptr);
    EXPECT_EQ(clux_type_get_kind(c.type), c.kind);
    EXPECT_STREQ(clux_type_get_name(c.type), c.name);
    EXPECT_EQ(clux_type_get_size(c.type), c.size);
    EXPECT_EQ(clux_type_get_align(c.type), c.size);
    clux_vtable_t *vt = clux_type_get_vtable(c.type);
    ASSERT_NE(vt, nullptr);
    EXPECT_NE(vt->add, nullptr);
    EXPECT_NE(vt->sub, nullptr);
    EXPECT_NE(vt->mul, nullptr);
    EXPECT_NE(vt->div, nullptr);
    EXPECT_NE(vt->mod, nullptr);
    EXPECT_NE(vt->shl, nullptr);
    EXPECT_NE(vt->shr, nullptr);
    EXPECT_NE(vt->bit_and, nullptr);
    EXPECT_NE(vt->bit_or, nullptr);
    EXPECT_NE(vt->bit_xor, nullptr);
    EXPECT_NE(vt->plus, nullptr);
    EXPECT_NE(vt->negative, nullptr);
    EXPECT_NE(vt->bit_not, nullptr);
    EXPECT_EQ(vt->logic_not, nullptr);
    EXPECT_NE(vt->safe_cast, nullptr);
  }

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CreateI8I16I64ValuesAndAdd) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *i8_left = clux_context_create_i8(ctx, 20, NULL, NULL);
  clux_value_t *i8_right = clux_context_create_i8(ctx, 22, NULL, NULL);
  clux_value_t *i8_sum = clux_context_add(ctx, i8_left, i8_right, NULL);
  ASSERT_NE(i8_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i8_sum), clux_context_get_i8_type(ctx));
  EXPECT_EQ(read_i8(i8_sum), 42);

  clux_value_t *i16_left = clux_context_create_i16(ctx, 200, NULL, NULL);
  clux_value_t *i16_right = clux_context_create_i16(ctx, 300, NULL, NULL);
  clux_value_t *i16_sum = clux_context_add(ctx, i16_left, i16_right, NULL);
  ASSERT_NE(i16_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i16_sum), clux_context_get_i16_type(ctx));
  EXPECT_EQ(read_i16(i16_sum), 500);

  clux_value_t *i64_left =
      clux_context_create_i64(ctx, INT64_C(4000000000), NULL, NULL);
  clux_value_t *i64_right =
      clux_context_create_i64(ctx, INT64_C(2000000000), NULL, NULL);
  clux_value_t *i64_sum = clux_context_add(ctx, i64_left, i64_right, NULL);
  ASSERT_NE(i64_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i64_sum), clux_context_get_i64_type(ctx));
  EXPECT_EQ(read_i64(i64_sum), INT64_C(6000000000));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BinaryWidensIntegerOperands) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *i8_value = clux_context_create_i8(ctx, 20, NULL, NULL);
  clux_value_t *i16_value = clux_context_create_i16(ctx, 300, NULL, NULL);
  clux_value_t *i16_sum =
      clux_context_binary(ctx, CLUX_OP_ADD, i8_value, i16_value, NULL);
  ASSERT_NE(i16_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i16_sum), clux_context_get_i16_type(ctx));
  EXPECT_EQ(read_i16(i16_sum), 320);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 100, NULL, NULL);
  clux_value_t *i64_value =
      clux_context_create_i64(ctx, INT64_C(4000000000), NULL, NULL);
  clux_value_t *i64_sum =
      clux_context_binary(ctx, CLUX_OP_ADD, i32_value, i64_value, NULL);
  ASSERT_NE(i64_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i64_sum), clux_context_get_i64_type(ctx));
  EXPECT_EQ(read_i64(i64_sum), INT64_C(4000000100));

  clux_value_t *i64_left =
      clux_context_create_i64(ctx, INT64_C(4000000000), NULL, NULL);
  clux_value_t *i32_right = clux_context_create_i32(ctx, 100, NULL, NULL);
  clux_value_t *i64_right_sum =
      clux_context_binary(ctx, CLUX_OP_ADD, i64_left, i32_right, NULL);
  ASSERT_NE(i64_right_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i64_right_sum), clux_context_get_i64_type(ctx));
  EXPECT_EQ(read_i64(i64_right_sum), INT64_C(4000000100));

  clux_value_t *shift_count = clux_context_create_i16(ctx, 2, NULL, NULL);
  clux_value_t *shifted =
      clux_context_binary(ctx, CLUX_OP_SHL, i8_value, shift_count, NULL);
  ASSERT_NE(shifted, nullptr);
  EXPECT_EQ(clux_value_get_type(shifted), clux_context_get_i16_type(ctx));
  EXPECT_EQ(read_i16(shifted), 80);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SafeCastWidensIntegers) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *i8_value = clux_context_create_i8(ctx, -5, NULL, NULL);
  clux_value_t *as_i16 = clux_context_safe_cast(
      ctx, i8_value, clux_context_get_i16_type(ctx), NULL);
  ASSERT_NE(as_i16, nullptr);
  EXPECT_EQ(clux_value_get_type(as_i16), clux_context_get_i16_type(ctx));
  EXPECT_EQ(read_i16(as_i16), -5);

  clux_value_t *as_i32 = clux_context_safe_cast(
      ctx, i8_value, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(as_i32, nullptr);
  EXPECT_EQ(clux_value_get_type(as_i32), clux_context_get_i32_type(ctx));
  EXPECT_EQ(read_i32(as_i32), -5);

  clux_value_t *as_i64 = clux_context_safe_cast(
      ctx, i8_value, clux_context_get_i64_type(ctx), NULL);
  ASSERT_NE(as_i64, nullptr);
  EXPECT_EQ(clux_value_get_type(as_i64), clux_context_get_i64_type(ctx));
  EXPECT_EQ(read_i64(as_i64), -5);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 123456, NULL, NULL);
  clux_value_t *i32_as_i64 = clux_context_safe_cast(
      ctx, i32_value, clux_context_get_i64_type(ctx), NULL);
  ASSERT_NE(i32_as_i64, nullptr);
  EXPECT_EQ(clux_value_get_type(i32_as_i64), clux_context_get_i64_type(ctx));
  EXPECT_EQ(read_i64(i32_as_i64), 123456);

  clux_value_t *identity = clux_context_safe_cast(
      ctx, i32_value, clux_context_get_i32_type(ctx), NULL);
  EXPECT_EQ(identity, i32_value);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SafeCastNarrowingProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  struct {
    clux_value_t *value;
    clux_type_t *target;
  } cases[] = {
      {clux_context_create_i16(ctx, 1, NULL, NULL),
       clux_context_get_i8_type(ctx)},
      {clux_context_create_i32(ctx, 1, NULL, NULL),
       clux_context_get_i16_type(ctx)},
      {clux_context_create_i64(ctx, 1, NULL, NULL),
       clux_context_get_i32_type(ctx)},
  };
  for (const auto &c : cases) {
    clux_value_t *result = clux_context_safe_cast(ctx, c.value, c.target, NULL);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
    EXPECT_NE(strstr(error_message_of(result), "narrowing conversion"),
              nullptr);
  }

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SafeCastToNonIntegerProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *result = clux_context_safe_cast(
      ctx, value, clux_context_get_error_type(ctx), NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(result), "cannot safely cast 'i32'"),
            nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SafeCastPropagatesErrorValue) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_error_t *error = create_error(a, NULL, "boom");
  clux_value_t *value = clux_context_create_error(ctx, error);
  clux_value_t *result =
      clux_context_safe_cast(ctx, value, clux_context_get_i32_type(ctx), NULL);
  EXPECT_EQ(result, value);
  EXPECT_NE(strstr(error_message_of(result), "boom"), nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SafeCastNullInputsProduceError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);

  clux_value_t *no_value =
      clux_context_safe_cast(ctx, NULL, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(no_value, nullptr);
  EXPECT_EQ(clux_value_get_type(no_value), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(no_value),
                   "safe cast requires a value and a target type"),
            nullptr);

  clux_value_t *no_target = clux_context_safe_cast(ctx, value, NULL, NULL);
  ASSERT_NE(no_target, nullptr);
  EXPECT_EQ(clux_value_get_type(no_target), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(no_target),
                   "safe cast requires a value and a target type"),
            nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BinaryArithmeticOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *left = clux_context_create_i32(ctx, 20, NULL, NULL);
  clux_value_t *right = clux_context_create_i32(ctx, 6, NULL, NULL);

  struct {
    clux_binary_op_t op;
    int32_t expected;
  } cases[] = {
      {CLUX_OP_ADD, 26},
      {CLUX_OP_SUB, 14},
      {CLUX_OP_MUL, 120},
      {CLUX_OP_DIV, 3},
      {CLUX_OP_MOD, 2},
  };
  for (const auto &c : cases) {
    clux_value_t *result = clux_context_binary(ctx, c.op, left, right, NULL);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(clux_value_get_type(result), clux_context_get_i32_type(ctx));
    EXPECT_EQ(read_i32(result), c.expected);
  }

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BinaryBitwiseOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *left = clux_context_create_i32(ctx, 12, NULL, NULL);
  clux_value_t *right = clux_context_create_i32(ctx, 10, NULL, NULL);

  struct {
    clux_binary_op_t op;
    int32_t expected;
  } cases[] = {
      {CLUX_OP_AND, 8},
      {CLUX_OP_OR, 14},
      {CLUX_OP_XOR, 6},
  };
  for (const auto &c : cases) {
    clux_value_t *result = clux_context_binary(ctx, c.op, left, right, NULL);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(read_i32(result), c.expected);
  }

  clux_value_t *shift = clux_context_create_i32(ctx, 2, NULL, NULL);
  clux_value_t *shl = clux_context_binary(ctx, CLUX_OP_SHL, left, shift, NULL);
  ASSERT_NE(shl, nullptr);
  EXPECT_EQ(read_i32(shl), 48);
  clux_value_t *shr = clux_context_binary(ctx, CLUX_OP_SHR, left, shift, NULL);
  ASSERT_NE(shr, nullptr);
  EXPECT_EQ(read_i32(shr), 3);

  clux_value_t *negative = clux_context_create_i32(ctx, -8, NULL, NULL);
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *arithmetic =
      clux_context_binary(ctx, CLUX_OP_SHR, negative, one, NULL);
  ASSERT_NE(arithmetic, nullptr);
  EXPECT_EQ(read_i32(arithmetic), -4);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PrefixOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);

  clux_value_t *plus = clux_context_prefix(ctx, CLUX_OP_PLUS, five, NULL);
  ASSERT_NE(plus, nullptr);
  EXPECT_EQ(read_i32(plus), 5);
  clux_value_t *negative =
      clux_context_prefix(ctx, CLUX_OP_NEGATIVE, five, NULL);
  ASSERT_NE(negative, nullptr);
  EXPECT_EQ(read_i32(negative), -5);
  clux_value_t *bit_not = clux_context_prefix(ctx, CLUX_OP_NOT, five, NULL);
  ASSERT_NE(bit_not, nullptr);
  EXPECT_EQ(read_i32(bit_not), ~5);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, DivisionByZeroProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *left = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *zero = clux_context_create_i32(ctx, 0, NULL, NULL);

  clux_value_t *div = clux_context_binary(ctx, CLUX_OP_DIV, left, zero, NULL);
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(clux_value_get_type(div), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(div), "division by zero"), nullptr);

  clux_value_t *mod = clux_context_binary(ctx, CLUX_OP_MOD, left, zero, NULL);
  ASSERT_NE(mod, nullptr);
  EXPECT_EQ(clux_value_get_type(mod), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(mod), "division by zero"), nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ShiftCountOutOfRangeProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *too_large = clux_context_create_i32(ctx, 32, NULL, NULL);
  clux_value_t *negative = clux_context_create_i32(ctx, -1, NULL, NULL);

  clux_value_t *shl =
      clux_context_binary(ctx, CLUX_OP_SHL, one, too_large, NULL);
  ASSERT_NE(shl, nullptr);
  EXPECT_EQ(clux_value_get_type(shl), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(shl), "shift count out of range"), nullptr);

  clux_value_t *shr =
      clux_context_binary(ctx, CLUX_OP_SHR, one, negative, NULL);
  ASSERT_NE(shr, nullptr);
  EXPECT_EQ(clux_value_get_type(shr), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(shr), "shift count out of range"), nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, UnsupportedOpsProduceError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_vtable_t empty_vtable = {};
  clux_type_t *custom = create_clux_type(a,
                                         CLUX_TYPE_I8,
                                         "custom",
                                         sizeof(int8_t),
                                         alignof(int8_t),
                                         &empty_vtable);
  clux_value_t *left = clux_context_create_shadow_value(ctx, custom);
  clux_value_t *right = clux_context_create_shadow_value(ctx, custom);

  clux_value_t *sub = clux_context_binary(ctx, CLUX_OP_SUB, left, right, NULL);
  ASSERT_NE(sub, nullptr);
  EXPECT_EQ(clux_value_get_type(sub), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(sub), "does not support operator '-'"),
            nullptr);

  clux_value_t *negative =
      clux_context_prefix(ctx, CLUX_OP_NEGATIVE, left, NULL);
  ASSERT_NE(negative, nullptr);
  EXPECT_EQ(clux_value_get_type(negative), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(negative), "does not support operator '-'"),
            nullptr);

  /* 严格类型：'!' 仅适用于 bool，整数类型不支持。 */
  clux_value_t *i32_value = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *logic_not =
      clux_context_prefix(ctx, CLUX_OP_LNOT, i32_value, NULL);
  ASSERT_NE(logic_not, nullptr);
  EXPECT_EQ(clux_value_get_type(logic_not), clux_context_get_error_type(ctx));
  EXPECT_NE(
      strstr(error_message_of(logic_not), "does not support operator '!'"),
      nullptr);

  delete_clux_context(&ctx);
  delete_clux_type(&custom);
  delete_allocator(&a);
}

TEST(EngineTest, NullOperandsProduceError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);

  clux_value_t *binary = clux_context_binary(ctx, CLUX_OP_ADD, NULL, one, NULL);
  ASSERT_NE(binary, nullptr);
  EXPECT_EQ(clux_value_get_type(binary), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(binary), "requires two operands"), nullptr);

  clux_value_t *prefix = clux_context_prefix(ctx, CLUX_OP_NEGATIVE, NULL, NULL);
  ASSERT_NE(prefix, nullptr);
  EXPECT_EQ(clux_value_get_type(prefix), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(prefix), "requires one operand"), nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ShadowValueBasics) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));
  ASSERT_NE(shadow, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow));
  EXPECT_EQ(clux_value_get_data(shadow), 0);
  EXPECT_EQ(clux_value_get_type(shadow), clux_context_get_i32_type(ctx));

  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);
  EXPECT_FALSE(clux_value_is_shadow(value));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BinaryWithShadowProducesShadow) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 5, NULL, NULL);
  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));

  clux_value_t *left_shadow =
      clux_context_binary(ctx, CLUX_OP_ADD, shadow, value, NULL);
  ASSERT_NE(left_shadow, nullptr);
  EXPECT_EQ(clux_value_get_type(left_shadow), clux_context_get_i32_type(ctx));
  EXPECT_TRUE(clux_value_is_shadow(left_shadow));

  clux_value_t *right_shadow =
      clux_context_binary(ctx, CLUX_OP_MUL, value, shadow, NULL);
  ASSERT_NE(right_shadow, nullptr);
  EXPECT_EQ(clux_value_get_type(right_shadow), clux_context_get_i32_type(ctx));
  EXPECT_TRUE(clux_value_is_shadow(right_shadow));

  clux_value_t *both_shadow =
      clux_context_binary(ctx, CLUX_OP_SUB, shadow, shadow, NULL);
  ASSERT_NE(both_shadow, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(both_shadow));

  clux_value_t *i8_shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i8_type(ctx));
  clux_value_t *i16_value = clux_context_create_i16(ctx, 2, NULL, NULL);
  clux_value_t *widened =
      clux_context_binary(ctx, CLUX_OP_ADD, i8_shadow, i16_value, NULL);
  ASSERT_NE(widened, nullptr);
  EXPECT_EQ(clux_value_get_type(widened), clux_context_get_i16_type(ctx));
  EXPECT_TRUE(clux_value_is_shadow(widened));

  clux_value_t *i8_value = clux_context_create_i8(ctx, 2, NULL, NULL);
  clux_value_t *i16_shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i16_type(ctx));
  clux_value_t *widened_right =
      clux_context_binary(ctx, CLUX_OP_ADD, i8_value, i16_shadow, NULL);
  ASSERT_NE(widened_right, nullptr);
  EXPECT_EQ(clux_value_get_type(widened_right), clux_context_get_i16_type(ctx));
  EXPECT_TRUE(clux_value_is_shadow(widened_right));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PrefixWithShadowProducesShadow) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));

  clux_value_t *negative =
      clux_context_prefix(ctx, CLUX_OP_NEGATIVE, shadow, NULL);
  ASSERT_NE(negative, nullptr);
  EXPECT_EQ(clux_value_get_type(negative), clux_context_get_i32_type(ctx));
  EXPECT_TRUE(clux_value_is_shadow(negative));

  clux_value_t *bit_not = clux_context_prefix(ctx, CLUX_OP_NOT, shadow, NULL);
  ASSERT_NE(bit_not, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(bit_not));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SafeCastShadowPropagates) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i8_type(ctx));

  clux_value_t *widened =
      clux_context_safe_cast(ctx, shadow, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(widened, nullptr);
  EXPECT_EQ(clux_value_get_type(widened), clux_context_get_i32_type(ctx));
  EXPECT_TRUE(clux_value_is_shadow(widened));

  clux_value_t *i32_shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));
  clux_value_t *narrowed = clux_context_safe_cast(
      ctx, i32_shadow, clux_context_get_i8_type(ctx), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_value_get_type(narrowed), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(narrowed), "narrowing conversion"),
            nullptr);

  clux_value_t *identity = clux_context_safe_cast(
      ctx, i32_shadow, clux_context_get_i32_type(ctx), NULL);
  EXPECT_EQ(identity, i32_shadow);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ShadowStillChecksOperatorSupport) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_vtable_t empty_vtable = {};
  clux_type_t *custom = create_clux_type(a,
                                         CLUX_TYPE_I8,
                                         "custom",
                                         sizeof(int8_t),
                                         alignof(int8_t),
                                         &empty_vtable);
  clux_value_t *left = clux_context_create_shadow_value(ctx, custom);
  clux_value_t *right = clux_context_create_shadow_value(ctx, custom);

  clux_value_t *result =
      clux_context_binary(ctx, CLUX_OP_ADD, left, right, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(result), "does not support operator '+'"),
            nullptr);

  delete_clux_context(&ctx);
  delete_clux_type(&custom);
  delete_allocator(&a);
}

TEST(EngineTest, CreateI32ValueAndReadData) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 42, "answer", NULL);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value), clux_context_get_i32_type(ctx));
  EXPECT_EQ(read_i32(value), 42);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ScopeDefineGetAndShadowing) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *outer = clux_context_create_i32(ctx, 1, "x", NULL);
  ASSERT_NE(outer, nullptr);

  clux_context_push_scope(ctx);
  clux_value_t *inner = clux_context_create_i32(ctx, 2, "x", NULL);
  ASSERT_NE(inner, nullptr);

  clux_context_pop_scope(ctx);
  clux_context_push_scope(ctx);
  /* pop 后新 scope 里看不到 inner，但能看到 outer。 */
  clux_value_t *shadow = clux_context_create_i32(ctx, 3, "x", NULL);
  ASSERT_NE(shadow, nullptr);
  clux_context_pop_scope(ctx);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, DuplicateDeclarationProducesErrorValue) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *first = clux_context_create_i32(ctx, 1, "x", NULL);
  ASSERT_NE(first, nullptr);
  clux_value_t *second = clux_context_create_i32(ctx, 2, "x", NULL);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(clux_value_get_type(second), clux_context_get_error_type(ctx));

  clux_error_t **stored =
      (clux_error_t **)ptr_to_raw(clux_value_get_data(second), 0);
  ASSERT_NE(stored, nullptr);
  EXPECT_NE(strstr(error_get_message(*stored), "duplicate declaration 'x'"),
            nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, AddI32Values) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *left = clux_context_create_i32(ctx, 20, NULL, NULL);
  clux_value_t *right = clux_context_create_i32(ctx, 22, NULL, NULL);
  clux_value_t *sum = clux_context_add(ctx, left, right, NULL);
  ASSERT_NE(sum, nullptr);
  EXPECT_EQ(clux_value_get_type(sum), clux_context_get_i32_type(ctx));
  EXPECT_EQ(read_i32(sum), 42);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, AddPropagatesLeftErrorOperand) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  location_t loc;
  memset(&loc, 0, sizeof(loc));
  loc.filename = "test.cx";
  clux_error_t *error = create_error(a, &loc, "boom");
  clux_value_t *left = clux_context_create_error(ctx, error);
  clux_value_t *right = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *result = clux_context_add(ctx, left, right, &loc);
  EXPECT_EQ(result, left);
  EXPECT_NE(strstr(error_message_of(result), "boom"), nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, AddPropagatesRightErrorOperand) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  location_t loc;
  memset(&loc, 0, sizeof(loc));
  loc.filename = "test.cx";
  clux_error_t *error = create_error(a, &loc, "bang");
  clux_value_t *left = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *right = clux_context_create_error(ctx, error);
  clux_value_t *result = clux_context_add(ctx, left, right, &loc);
  EXPECT_EQ(result, right);
  EXPECT_NE(strstr(error_message_of(result), "bang"), nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PrefixPropagatesErrorValue) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_error_t *error = create_error(a, NULL, "boom");
  clux_value_t *value = clux_context_create_error(ctx, error);
  clux_value_t *result =
      clux_context_prefix(ctx, CLUX_OP_NEGATIVE, value, NULL);
  EXPECT_EQ(result, value);
  EXPECT_NE(strstr(error_message_of(result), "boom"), nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, AddNullOperandProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *right = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *result = clux_context_add(ctx, NULL, right, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
  clux_error_t **stored =
      (clux_error_t **)ptr_to_raw(clux_value_get_data(result), 0);
  ASSERT_NE(stored, nullptr);
  EXPECT_NE(strstr(error_get_message(*stored), "requires two operands"),
            nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, TypeMetaLifecycle) {
  allocator_t *a = create_allocator(malloc, free);
  clux_type_t *type = create_clux_type(a, CLUX_TYPE_I32, "meta", 4, 4, NULL);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(clux_type_get_meta(type), nullptr);
  void *first = clux_type_create_meta(type, 8);
  EXPECT_EQ(clux_type_get_meta(type), first);
  void *second = clux_type_create_meta(type, 16);
  EXPECT_EQ(clux_type_get_meta(type), second);
  delete_clux_type(&type);
  delete_allocator(&a);
}

TEST(EngineTest, NoLeak) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *left = clux_context_create_i32(ctx, 1, "a", NULL);
  clux_value_t *right = clux_context_create_i32(ctx, 2, "b", NULL);
  clux_value_t *sum = clux_context_add(ctx, left, right, NULL);
  ASSERT_NE(sum, nullptr);
  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));
  clux_value_t *shadow_sum = clux_context_add(ctx, shadow, right, NULL);
  ASSERT_NE(shadow_sum, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_sum));
  clux_context_push_scope(ctx);
  clux_context_create_i32(ctx, 3, "a", NULL);
  clux_context_pop_scope(ctx);
  delete_clux_context(&ctx);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
