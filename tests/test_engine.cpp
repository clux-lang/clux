#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/integer.h"
#include "engine/interface.h"
#include "engine/ptr.h"
#include "engine/type.h"
#include "engine/value.h"
#include <cmath>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>

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

static uint8_t read_u8(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(uint8_t *)raw : 0;
}

static uint16_t read_u16(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(uint16_t *)raw : 0;
}

static uint32_t read_u32(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(uint32_t *)raw : 0;
}

static uint64_t read_u64(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(uint64_t *)raw : 0;
}

static intptr_t read_isize(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(intptr_t *)raw : 0;
}

static size_t read_usize(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(size_t *)raw : 0;
}

static float read_f32(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(float *)raw : 0;
}

static double read_f64(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(double *)raw : 0;
}

static bool read_bool(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(bool *)raw : false;
}

static const char *read_str(clux_value_t *value) {
  char **raw = (char **)ptr_to_raw(clux_value_get_data(value), 0);
  return (raw && *raw) ? *raw : "";
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
    EXPECT_NE(vt->eq, nullptr);
    EXPECT_NE(vt->ne, nullptr);
    EXPECT_NE(vt->lt, nullptr);
    EXPECT_NE(vt->le, nullptr);
    EXPECT_NE(vt->gt, nullptr);
    EXPECT_NE(vt->ge, nullptr);
    EXPECT_NE(vt->plus, nullptr);
    EXPECT_NE(vt->negative, nullptr);
    EXPECT_NE(vt->bit_not, nullptr);
    EXPECT_EQ(vt->logic_not, nullptr);
    EXPECT_NE(vt->safe_cast, nullptr);
    EXPECT_NE(vt->cast, nullptr);
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

TEST(EngineTest, PlatformIsizeAndUsizeTypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  /* 元信息：宽度跟随平台指针宽度 */
  clux_type_t *isize_type = clux_context_get_isize_type(ctx);
  clux_type_t *usize_type = clux_context_get_usize_type(ctx);
  ASSERT_NE(isize_type, nullptr);
  ASSERT_NE(usize_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(isize_type), CLUX_TYPE_ISIZE);
  EXPECT_EQ(clux_type_get_kind(usize_type), CLUX_TYPE_USIZE);
  EXPECT_STREQ(clux_type_get_name(isize_type), "isize");
  EXPECT_STREQ(clux_type_get_name(usize_type), "usize");
  EXPECT_EQ(clux_type_get_size(isize_type), sizeof(intptr_t));
  EXPECT_EQ(clux_type_get_size(usize_type), sizeof(size_t));
  EXPECT_EQ(clux_type_get_align(isize_type), sizeof(intptr_t));
  EXPECT_EQ(clux_type_get_align(usize_type), sizeof(size_t));
  EXPECT_EQ(clux_context_get_isize_type(ctx), isize_type);
  EXPECT_EQ(clux_context_get_usize_type(ctx), usize_type);

  /* 算术：同类型相加保类型 */
  clux_value_t *isize_left = clux_context_create_isize(ctx, -100, NULL, NULL);
  clux_value_t *isize_right = clux_context_create_isize(ctx, 142, NULL, NULL);
  clux_value_t *isize_sum =
      clux_context_add(ctx, isize_left, isize_right, NULL);
  ASSERT_NE(isize_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(isize_sum), isize_type);
  EXPECT_EQ(read_isize(isize_sum), 42);

  clux_value_t *usize_left =
      clux_context_create_usize(ctx, (size_t)3000000000, NULL, NULL);
  clux_value_t *usize_right =
      clux_context_create_usize(ctx, (size_t)1000000000, NULL, NULL);
  clux_value_t *usize_sum =
      clux_context_add(ctx, usize_left, usize_right, NULL);
  ASSERT_NE(usize_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(usize_sum), usize_type);
  if (sizeof(size_t) > 4) {
    EXPECT_EQ(read_usize(usize_sum), (size_t)4000000000);
  }

  /* 混合协商：i32 与 isize 运算取宽者 isize */
  clux_value_t *i32_value = clux_context_create_i32(ctx, 20, NULL, NULL);
  clux_value_t *mixed_sum = clux_context_add(ctx, i32_value, isize_left, NULL);
  ASSERT_NE(mixed_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(mixed_sum), isize_type);
  EXPECT_EQ(read_isize(mixed_sum), -80);

  /* safe_cast：同宽互转可行，narrowing 拒绝 */
  clux_value_t *i64_value = clux_context_create_i64(ctx, 12345, NULL, NULL);
  clux_value_t *to_isize =
      clux_context_safe_cast(ctx, i64_value, isize_type, NULL);
  ASSERT_NE(to_isize, nullptr);
  EXPECT_EQ(clux_value_get_type(to_isize), isize_type);
  EXPECT_EQ(read_isize(to_isize), 12345);

  clux_value_t *narrowed = clux_context_safe_cast(
      ctx, isize_left, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(narrowed)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(narrowed),
               "cannot safely cast 'isize' to 'i32': narrowing conversion");

  /* cast 矩阵：bool→usize、f64→isize、usize→u32 显式截断 */
  clux_value_t *bool_value = clux_context_create_bool(ctx, true, NULL, NULL);
  clux_value_t *bool_to_usize =
      clux_context_cast(ctx, bool_value, usize_type, NULL);
  ASSERT_NE(bool_to_usize, nullptr);
  EXPECT_EQ(clux_value_get_type(bool_to_usize), usize_type);
  EXPECT_EQ(read_usize(bool_to_usize), (size_t)1);

  clux_value_t *f64_value = clux_context_create_f64(ctx, -3.75, NULL, NULL);
  clux_value_t *f64_to_isize =
      clux_context_cast(ctx, f64_value, isize_type, NULL);
  ASSERT_NE(f64_to_isize, nullptr);
  EXPECT_EQ(clux_value_get_type(f64_to_isize), isize_type);
  EXPECT_EQ(read_isize(f64_to_isize), -3);

  clux_value_t *usize_to_u32 =
      clux_context_cast(ctx, usize_left, clux_context_get_u32_type(ctx), NULL);
  ASSERT_NE(usize_to_u32, nullptr);
  EXPECT_EQ(read_u32(usize_to_u32), (uint32_t)3000000000u);

  /* 关系运算 */
  clux_value_t *cmp =
      clux_context_binary(ctx, CLUX_OP_LT, isize_left, isize_right, NULL);
  ASSERT_NE(cmp, nullptr);
  EXPECT_EQ(read_bool(cmp), true);

  /* 非数值混合报错 */
  clux_value_t *str_value = clux_context_create_str(ctx, "x", NULL, NULL);
  clux_value_t *bad = clux_context_add(ctx, isize_left, str_value, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "operator '+' requires operands of the same type");

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

TEST(EngineTest, BuiltinUnsignedTypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  struct {
    clux_type_t *type;
    clux_type_kind_t kind;
    const char *name;
    size_t size;
  } cases[] = {
      {clux_context_get_u8_type(ctx), CLUX_TYPE_U8, "u8", sizeof(uint8_t)},
      {clux_context_get_u16_type(ctx), CLUX_TYPE_U16, "u16", sizeof(uint16_t)},
      {clux_context_get_u32_type(ctx), CLUX_TYPE_U32, "u32", sizeof(uint32_t)},
      {clux_context_get_u64_type(ctx), CLUX_TYPE_U64, "u64", sizeof(uint64_t)},
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
    EXPECT_NE(vt->eq, nullptr);
    EXPECT_NE(vt->ne, nullptr);
    EXPECT_NE(vt->lt, nullptr);
    EXPECT_NE(vt->le, nullptr);
    EXPECT_NE(vt->gt, nullptr);
    EXPECT_NE(vt->ge, nullptr);
    EXPECT_NE(vt->plus, nullptr);
    EXPECT_NE(vt->negative, nullptr);
    EXPECT_NE(vt->bit_not, nullptr);
    EXPECT_EQ(vt->logic_not, nullptr);
    EXPECT_NE(vt->safe_cast, nullptr);
    EXPECT_NE(vt->cast, nullptr);
  }

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CreateUnsignedValuesAndAdd) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *u8_left = clux_context_create_u8(ctx, 200, NULL, NULL);
  clux_value_t *u8_right = clux_context_create_u8(ctx, 55, NULL, NULL);
  clux_value_t *u8_sum = clux_context_add(ctx, u8_left, u8_right, NULL);
  ASSERT_NE(u8_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u8_sum), clux_context_get_u8_type(ctx));
  EXPECT_EQ(read_u8(u8_sum), 255);

  clux_value_t *u16_left = clux_context_create_u16(ctx, 60000, NULL, NULL);
  clux_value_t *u16_right = clux_context_create_u16(ctx, 5000, NULL, NULL);
  clux_value_t *u16_sum = clux_context_add(ctx, u16_left, u16_right, NULL);
  ASSERT_NE(u16_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u16_sum), clux_context_get_u16_type(ctx));
  EXPECT_EQ(read_u16(u16_sum), 65000);

  clux_value_t *u32_left =
      clux_context_create_u32(ctx, UINT32_C(4000000000), NULL, NULL);
  clux_value_t *u32_right = clux_context_create_u32(ctx, 200, NULL, NULL);
  clux_value_t *u32_sum = clux_context_add(ctx, u32_left, u32_right, NULL);
  ASSERT_NE(u32_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u32_sum), clux_context_get_u32_type(ctx));
  EXPECT_EQ(read_u32(u32_sum), UINT32_C(4000000200));

  clux_value_t *u64_left =
      clux_context_create_u64(ctx, UINT64_C(10000000000), NULL, NULL);
  clux_value_t *u64_right =
      clux_context_create_u64(ctx, UINT64_C(20000000000), NULL, NULL);
  clux_value_t *u64_sum = clux_context_add(ctx, u64_left, u64_right, NULL);
  ASSERT_NE(u64_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u64_sum), clux_context_get_u64_type(ctx));
  EXPECT_EQ(read_u64(u64_sum), UINT64_C(30000000000));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, UnsignedWidensIntegerOperands) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *u8_value = clux_context_create_u8(ctx, 20, NULL, NULL);
  clux_value_t *u16_value = clux_context_create_u16(ctx, 300, NULL, NULL);
  clux_value_t *u16_sum =
      clux_context_binary(ctx, CLUX_OP_ADD, u8_value, u16_value, NULL);
  ASSERT_NE(u16_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u16_sum), clux_context_get_u16_type(ctx));
  EXPECT_EQ(read_u16(u16_sum), 320);

  clux_value_t *u32_value = clux_context_create_u32(ctx, 100, NULL, NULL);
  clux_value_t *u64_value =
      clux_context_create_u64(ctx, UINT64_C(4000000000), NULL, NULL);
  clux_value_t *u64_sum =
      clux_context_binary(ctx, CLUX_OP_ADD, u32_value, u64_value, NULL);
  ASSERT_NE(u64_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u64_sum), clux_context_get_u64_type(ctx));
  EXPECT_EQ(read_u64(u64_sum), UINT64_C(4000000100));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, UnsignedArithmeticAndBitwiseOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *zero = clux_context_create_u8(ctx, 0, NULL, NULL);
  clux_value_t *one = clux_context_create_u8(ctx, 1, NULL, NULL);
  clux_value_t *wrapped =
      clux_context_binary(ctx, CLUX_OP_SUB, zero, one, NULL);
  ASSERT_NE(wrapped, nullptr);
  EXPECT_EQ(read_u8(wrapped), 255);

  clux_value_t *left = clux_context_create_u32(ctx, 12, NULL, NULL);
  clux_value_t *right = clux_context_create_u32(ctx, 10, NULL, NULL);
  struct {
    clux_binary_op_t op;
    uint32_t expected;
  } cases[] = {
      {CLUX_OP_AND, 8},
      {CLUX_OP_OR, 14},
      {CLUX_OP_XOR, 6},
  };
  for (const auto &c : cases) {
    clux_value_t *result = clux_context_binary(ctx, c.op, left, right, NULL);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(read_u32(result), c.expected);
  }

  clux_value_t *shift = clux_context_create_u32(ctx, 2, NULL, NULL);
  clux_value_t *shl = clux_context_binary(ctx, CLUX_OP_SHL, left, shift, NULL);
  ASSERT_NE(shl, nullptr);
  EXPECT_EQ(read_u32(shl), 48);
  clux_value_t *high = clux_context_create_u8(ctx, 0x80, NULL, NULL);
  clux_value_t *shr = clux_context_binary(ctx, CLUX_OP_SHR, high, one, NULL);
  ASSERT_NE(shr, nullptr);
  EXPECT_EQ(read_u8(shr), 0x40);

  clux_value_t *negative =
      clux_context_prefix(ctx, CLUX_OP_NEGATIVE, one, NULL);
  ASSERT_NE(negative, nullptr);
  EXPECT_EQ(read_u8(negative), 255);
  clux_value_t *bit_not = clux_context_prefix(ctx, CLUX_OP_NOT, one, NULL);
  ASSERT_NE(bit_not, nullptr);
  EXPECT_EQ(read_u8(bit_not), 254);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, UnsignedSafeCast) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *u8_value = clux_context_create_u8(ctx, 200, NULL, NULL);
  clux_value_t *widened = clux_context_safe_cast(
      ctx, u8_value, clux_context_get_u32_type(ctx), NULL);
  ASSERT_NE(widened, nullptr);
  EXPECT_EQ(clux_value_get_type(widened), clux_context_get_u32_type(ctx));
  EXPECT_EQ(read_u32(widened), 200);

  clux_value_t *narrowed =
      clux_context_safe_cast(ctx, widened, clux_context_get_u8_type(ctx), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_value_get_type(narrowed), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(narrowed), "narrowing conversion"),
            nullptr);

  clux_value_t *to_signed = clux_context_safe_cast(
      ctx, u8_value, clux_context_get_i64_type(ctx), NULL);
  ASSERT_NE(to_signed, nullptr);
  EXPECT_EQ(clux_value_get_type(to_signed), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(to_signed), "cannot safely cast 'u8'"),
            nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, MixedSignedAndUnsignedProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *u32_value = clux_context_create_u32(ctx, 1, NULL, NULL);
  clux_value_t *left_signed =
      clux_context_binary(ctx, CLUX_OP_ADD, i32_value, u32_value, NULL);
  ASSERT_NE(left_signed, nullptr);
  EXPECT_EQ(clux_value_get_type(left_signed), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(left_signed),
                   "requires operands of the same type"),
            nullptr);

  clux_value_t *left_unsigned =
      clux_context_binary(ctx, CLUX_OP_ADD, u32_value, i32_value, NULL);
  ASSERT_NE(left_unsigned, nullptr);
  EXPECT_EQ(clux_value_get_type(left_unsigned),
            clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(left_unsigned),
                   "requires operands of the same type"),
            nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, UnsignedShadowPropagates) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_u8_type(ctx));
  clux_value_t *value = clux_context_create_u16(ctx, 2, NULL, NULL);
  clux_value_t *result =
      clux_context_binary(ctx, CLUX_OP_ADD, shadow, value, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_u16_type(ctx));
  EXPECT_TRUE(clux_value_is_shadow(result));
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, UnsignedDivisionByZeroProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *one = clux_context_create_u32(ctx, 1, NULL, NULL);
  clux_value_t *zero = clux_context_create_u32(ctx, 0, NULL, NULL);
  clux_value_t *result = clux_context_binary(ctx, CLUX_OP_DIV, one, zero, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(result), "division by zero"), nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CastNarrowsAndCrossesSignedness) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 42, NULL, NULL);
  clux_value_t *as_i8 =
      clux_context_cast(ctx, i32_value, clux_context_get_i8_type(ctx), NULL);
  ASSERT_NE(as_i8, nullptr);
  EXPECT_EQ(clux_value_get_type(as_i8), clux_context_get_i8_type(ctx));
  EXPECT_EQ(read_i8(as_i8), 42);

  clux_value_t *minus_one = clux_context_create_i32(ctx, -1, NULL, NULL);
  clux_value_t *as_u8 =
      clux_context_cast(ctx, minus_one, clux_context_get_u8_type(ctx), NULL);
  ASSERT_NE(as_u8, nullptr);
  EXPECT_EQ(clux_value_get_type(as_u8), clux_context_get_u8_type(ctx));
  EXPECT_EQ(read_u8(as_u8), 255);

  clux_value_t *as_u32 =
      clux_context_cast(ctx, minus_one, clux_context_get_u32_type(ctx), NULL);
  ASSERT_NE(as_u32, nullptr);
  EXPECT_EQ(clux_value_get_type(as_u32), clux_context_get_u32_type(ctx));
  EXPECT_EQ(read_u32(as_u32), UINT32_MAX);

  clux_value_t *u8_value = clux_context_create_u8(ctx, 255, NULL, NULL);
  clux_value_t *as_i8_from_u8 =
      clux_context_cast(ctx, u8_value, clux_context_get_i8_type(ctx), NULL);
  ASSERT_NE(as_i8_from_u8, nullptr);
  EXPECT_EQ(read_i8(as_i8_from_u8), -1);

  clux_value_t *u32_value =
      clux_context_create_u32(ctx, UINT32_C(300), NULL, NULL);
  clux_value_t *as_u8_from_u32 =
      clux_context_cast(ctx, u32_value, clux_context_get_u8_type(ctx), NULL);
  ASSERT_NE(as_u8_from_u32, nullptr);
  EXPECT_EQ(read_u8(as_u8_from_u32), 44);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CastShadowPropagates) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i8_type(ctx));
  clux_value_t *result =
      clux_context_cast(ctx, shadow, clux_context_get_u64_type(ctx), NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_u64_type(ctx));
  EXPECT_TRUE(clux_value_is_shadow(result));
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CastPropagatesErrorValue) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_error_t *error = create_error(a, NULL, "boom");
  clux_value_t *value = clux_context_create_error(ctx, error);
  clux_value_t *result =
      clux_context_cast(ctx, value, clux_context_get_i32_type(ctx), NULL);
  EXPECT_EQ(result, value);
  EXPECT_NE(strstr(error_message_of(result), "boom"), nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CastNonIntegerTargetProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *result =
      clux_context_cast(ctx, value, clux_context_get_error_type(ctx), NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(result), "cannot cast 'i32' to 'error'"),
            nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CastNullInputsProduceError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);

  clux_value_t *no_value =
      clux_context_cast(ctx, NULL, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(no_value, nullptr);
  EXPECT_EQ(clux_value_get_type(no_value), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(no_value),
                   "cast requires a value and a target type"),
            nullptr);

  clux_value_t *no_target = clux_context_cast(ctx, value, NULL, NULL);
  ASSERT_NE(no_target, nullptr);
  EXPECT_EQ(clux_value_get_type(no_target), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(no_target),
                   "cast requires a value and a target type"),
            nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CastIdentityReturnsSameValue) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *result =
      clux_context_cast(ctx, value, clux_context_get_i32_type(ctx), NULL);
  EXPECT_EQ(result, value);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BuiltinFloatingTypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  struct {
    clux_type_t *type;
    clux_type_kind_t kind;
    const char *name;
    size_t size;
  } cases[] = {
      {clux_context_get_f32_type(ctx), CLUX_TYPE_F32, "f32", sizeof(float)},
      {clux_context_get_f64_type(ctx), CLUX_TYPE_F64, "f64", sizeof(double)},
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
    EXPECT_EQ(vt->mod, nullptr);
    EXPECT_NE(vt->plus, nullptr);
    EXPECT_NE(vt->negative, nullptr);
    EXPECT_EQ(vt->shl, nullptr);
    EXPECT_EQ(vt->shr, nullptr);
    EXPECT_EQ(vt->bit_and, nullptr);
    EXPECT_EQ(vt->bit_or, nullptr);
    EXPECT_EQ(vt->bit_xor, nullptr);
    EXPECT_EQ(vt->bit_not, nullptr);
    EXPECT_EQ(vt->logic_not, nullptr);
    EXPECT_NE(vt->safe_cast, nullptr);
    EXPECT_NE(vt->cast, nullptr);
  }

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatingArithmeticOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *f32_left = clux_context_create_f32(ctx, 1.5f, NULL, NULL);
  clux_value_t *f32_right = clux_context_create_f32(ctx, 2.25f, NULL, NULL);
  clux_value_t *f32_sum = clux_context_add(ctx, f32_left, f32_right, NULL);
  ASSERT_NE(f32_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(f32_sum), clux_context_get_f32_type(ctx));
  EXPECT_FLOAT_EQ(read_f32(f32_sum), 3.75f);

  clux_value_t *f64_left = clux_context_create_f64(ctx, 7.5, NULL, NULL);
  clux_value_t *f64_right = clux_context_create_f64(ctx, 2.5, NULL, NULL);
  struct {
    clux_binary_op_t op;
    double expected;
  } cases[] = {
      {CLUX_OP_SUB, 5.0},
      {CLUX_OP_MUL, 18.75},
      {CLUX_OP_DIV, 3.0},
  };
  for (const auto &c : cases) {
    clux_value_t *result =
        clux_context_binary(ctx, c.op, f64_left, f64_right, nullptr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(clux_value_get_type(result), clux_context_get_f64_type(ctx));
    EXPECT_DOUBLE_EQ(read_f64(result), c.expected);
  }

  clux_value_t *negative =
      clux_context_prefix(ctx, CLUX_OP_NEGATIVE, f64_left, NULL);
  ASSERT_NE(negative, nullptr);
  EXPECT_DOUBLE_EQ(read_f64(negative), -7.5);
  clux_value_t *plus = clux_context_prefix(ctx, CLUX_OP_PLUS, f64_left, NULL);
  ASSERT_NE(plus, nullptr);
  EXPECT_DOUBLE_EQ(read_f64(plus), 7.5);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatWidensToDouble) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *f32_value = clux_context_create_f32(ctx, 1.5f, NULL, NULL);
  clux_value_t *f64_value = clux_context_create_f64(ctx, 2.25, NULL, NULL);
  clux_value_t *sum = clux_context_add(ctx, f32_value, f64_value, NULL);
  ASSERT_NE(sum, nullptr);
  EXPECT_EQ(clux_value_get_type(sum), clux_context_get_f64_type(ctx));
  EXPECT_DOUBLE_EQ(read_f64(sum), 3.75);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatDivisionByZeroFollowsIEEE) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *left = clux_context_create_f64(ctx, 1.0, NULL, NULL);
  clux_value_t *right = clux_context_create_f64(ctx, 0.0, NULL, NULL);
  clux_value_t *quotient =
      clux_context_binary(ctx, CLUX_OP_DIV, left, right, NULL);
  ASSERT_NE(quotient, nullptr);
  EXPECT_EQ(clux_value_get_type(quotient), clux_context_get_f64_type(ctx));
  EXPECT_TRUE(std::isinf(read_f64(quotient)));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatSafeCastWidensOnly) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *f32_value = clux_context_create_f32(ctx, 1.5f, NULL, NULL);
  clux_value_t *widened = clux_context_safe_cast(
      ctx, f32_value, clux_context_get_f64_type(ctx), NULL);
  ASSERT_NE(widened, nullptr);
  EXPECT_EQ(clux_value_get_type(widened), clux_context_get_f64_type(ctx));
  EXPECT_DOUBLE_EQ(read_f64(widened), 1.5);

  clux_value_t *f64_value = clux_context_create_f64(ctx, 1.5, NULL, NULL);
  clux_value_t *narrowed = clux_context_safe_cast(
      ctx, f64_value, clux_context_get_f32_type(ctx), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(narrowed)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(narrowed),
               "cannot safely cast 'f64' to 'f32': narrowing conversion");

  clux_value_t *to_int = clux_context_safe_cast(
      ctx, f64_value, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(to_int, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(to_int)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(to_int), "cannot safely cast 'f64' to 'i32'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatCastCrossesCategories) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *f64_value = clux_context_create_f64(ctx, 2.5, NULL, NULL);
  clux_value_t *to_i32 =
      clux_context_cast(ctx, f64_value, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(to_i32, nullptr);
  EXPECT_EQ(clux_value_get_type(to_i32), clux_context_get_i32_type(ctx));
  EXPECT_EQ(read_i32(to_i32), 2);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 7, NULL, NULL);
  clux_value_t *to_f64 =
      clux_context_cast(ctx, i32_value, clux_context_get_f64_type(ctx), NULL);
  ASSERT_NE(to_f64, nullptr);
  EXPECT_EQ(clux_value_get_type(to_f64), clux_context_get_f64_type(ctx));
  EXPECT_DOUBLE_EQ(read_f64(to_f64), 7.0);

  clux_value_t *u8_value = clux_context_create_u8(ctx, 255, NULL, NULL);
  clux_value_t *u8_to_f32 =
      clux_context_cast(ctx, u8_value, clux_context_get_f32_type(ctx), NULL);
  ASSERT_NE(u8_to_f32, nullptr);
  EXPECT_EQ(clux_value_get_type(u8_to_f32), clux_context_get_f32_type(ctx));
  EXPECT_FLOAT_EQ(read_f32(u8_to_f32), 255.0f);

  clux_value_t *narrowed =
      clux_context_cast(ctx, f64_value, clux_context_get_f32_type(ctx), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_value_get_type(narrowed), clux_context_get_f32_type(ctx));
  EXPECT_FLOAT_EQ(read_f32(narrowed), 2.5f);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatShadowAndErrorPropagate) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *shadow_f32 =
      clux_context_create_shadow_value(ctx, clux_context_get_f32_type(ctx));
  clux_value_t *f64_value = clux_context_create_f64(ctx, 1.0, NULL, NULL);
  clux_value_t *sum = clux_context_add(ctx, shadow_f32, f64_value, NULL);
  ASSERT_NE(sum, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(sum));
  EXPECT_EQ(clux_value_get_type(sum), clux_context_get_f64_type(ctx));

  clux_value_t *error = clux_context_create_error_message(ctx, NULL, "boom");
  clux_value_t *propagated = clux_context_add(ctx, error, f64_value, NULL);
  EXPECT_EQ(propagated, error);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatModBitwiseAndShiftUnsupported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *left = clux_context_create_f64(ctx, 3.0, NULL, NULL);
  clux_value_t *right = clux_context_create_f64(ctx, 1.0, NULL, NULL);
  struct {
    clux_binary_op_t op;
    const char *symbol;
  } cases[] = {
      {CLUX_OP_MOD, "%"},
      {CLUX_OP_AND, "&"},
      {CLUX_OP_OR, "|"},
      {CLUX_OP_XOR, "^"},
      {CLUX_OP_SHL, "<<"},
      {CLUX_OP_SHR, ">>"},
  };
  for (const auto &c : cases) {
    clux_value_t *result = clux_context_binary(ctx, c.op, left, right, nullptr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
    std::string expected =
        "type 'f64' does not support operator '" + std::string(c.symbol) + "'";
    EXPECT_STREQ(error_message_of(result), expected.c_str());
  }

  clux_value_t *bit_not = clux_context_prefix(ctx, CLUX_OP_NOT, left, NULL);
  ASSERT_NE(bit_not, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bit_not)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bit_not),
               "type 'f64' does not support operator '~'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BuiltinBoolType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *type = clux_context_get_bool_type(ctx);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(clux_type_get_kind(type), CLUX_TYPE_BOOL);
  EXPECT_STREQ(clux_type_get_name(type), "bool");
  EXPECT_EQ(clux_type_get_size(type), sizeof(bool));
  EXPECT_EQ(clux_type_get_align(type), alignof(bool));
  clux_vtable_t *vt = clux_type_get_vtable(type);
  ASSERT_NE(vt, nullptr);
  EXPECT_EQ(vt->add, nullptr);
  EXPECT_EQ(vt->sub, nullptr);
  EXPECT_EQ(vt->mul, nullptr);
  EXPECT_EQ(vt->div, nullptr);
  EXPECT_EQ(vt->mod, nullptr);
  EXPECT_EQ(vt->shl, nullptr);
  EXPECT_EQ(vt->shr, nullptr);
  EXPECT_EQ(vt->bit_and, nullptr);
  EXPECT_EQ(vt->bit_or, nullptr);
  EXPECT_EQ(vt->bit_xor, nullptr);
  EXPECT_EQ(vt->plus, nullptr);
  EXPECT_EQ(vt->negative, nullptr);
  EXPECT_EQ(vt->bit_not, nullptr);
  EXPECT_NE(vt->logic_not, nullptr);
  EXPECT_NE(vt->eq, nullptr);
  EXPECT_NE(vt->ne, nullptr);
  EXPECT_EQ(vt->lt, nullptr);
  EXPECT_EQ(vt->le, nullptr);
  EXPECT_EQ(vt->gt, nullptr);
  EXPECT_EQ(vt->ge, nullptr);
  EXPECT_NE(vt->safe_cast, nullptr);
  EXPECT_NE(vt->cast, nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BoolLogicNot) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *truth = clux_context_create_bool(ctx, true, NULL, NULL);
  clux_value_t *negated = clux_context_prefix(ctx, CLUX_OP_LNOT, truth, NULL);
  ASSERT_NE(negated, nullptr);
  EXPECT_EQ(clux_value_get_type(negated), clux_context_get_bool_type(ctx));
  EXPECT_FALSE(read_bool(negated));

  clux_value_t *again = clux_context_prefix(ctx, CLUX_OP_LNOT, negated, NULL);
  ASSERT_NE(again, nullptr);
  EXPECT_TRUE(read_bool(again));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BoolCastToNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *truth = clux_context_create_bool(ctx, true, NULL, NULL);
  clux_value_t *to_i32 =
      clux_context_cast(ctx, truth, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(to_i32, nullptr);
  EXPECT_EQ(read_i32(to_i32), 1);

  clux_value_t *lie = clux_context_create_bool(ctx, false, NULL, NULL);
  clux_value_t *to_u8 =
      clux_context_cast(ctx, lie, clux_context_get_u8_type(ctx), NULL);
  ASSERT_NE(to_u8, nullptr);
  EXPECT_EQ(read_u8(to_u8), 0);

  clux_value_t *to_f64 =
      clux_context_cast(ctx, truth, clux_context_get_f64_type(ctx), NULL);
  ASSERT_NE(to_f64, nullptr);
  EXPECT_DOUBLE_EQ(read_f64(to_f64), 1.0);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, NumericCastToBool) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  struct {
    clux_value_t *value;
    bool expected;
  } cases[] = {
      {clux_context_create_i32(ctx, 0, NULL, NULL), false},
      {clux_context_create_i32(ctx, 5, NULL, NULL), true},
      {clux_context_create_i32(ctx, -1, NULL, NULL), true},
      {clux_context_create_u8(ctx, 0, NULL, NULL), false},
      {clux_context_create_u8(ctx, 1, NULL, NULL), true},
      {clux_context_create_f64(ctx, 0.0, NULL, NULL), false},
      {clux_context_create_f64(ctx, 2.5, NULL, NULL), true},
  };
  for (const auto &c : cases) {
    clux_value_t *result =
        clux_context_cast(ctx, c.value, clux_context_get_bool_type(ctx), NULL);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(clux_value_get_type(result), clux_context_get_bool_type(ctx));
    EXPECT_EQ(read_bool(result), c.expected);
  }

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BoolSafeCastStrict) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *truth = clux_context_create_bool(ctx, true, NULL, NULL);
  clux_value_t *identity =
      clux_context_safe_cast(ctx, truth, clux_context_get_bool_type(ctx), NULL);
  EXPECT_EQ(identity, truth);

  clux_value_t *to_i32 =
      clux_context_safe_cast(ctx, truth, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(to_i32, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(to_i32)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(to_i32), "cannot safely cast 'bool' to 'i32'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BoolUnsupportedOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *left = clux_context_create_bool(ctx, true, NULL, NULL);
  clux_value_t *right = clux_context_create_bool(ctx, false, NULL, NULL);
  clux_value_t *sum = clux_context_add(ctx, left, right, NULL);
  ASSERT_NE(sum, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(sum)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(sum),
               "type 'bool' does not support operator '+'");

  clux_value_t *bit_and =
      clux_context_binary(ctx, CLUX_OP_AND, left, right, NULL);
  ASSERT_NE(bit_and, nullptr);
  EXPECT_STREQ(error_message_of(bit_and),
               "type 'bool' does not support operator '&'");

  clux_value_t *bit_not = clux_context_prefix(ctx, CLUX_OP_NOT, left, NULL);
  ASSERT_NE(bit_not, nullptr);
  EXPECT_STREQ(error_message_of(bit_not),
               "type 'bool' does not support operator '~'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BoolShadowAndErrorPropagate) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_bool_type(ctx));
  clux_value_t *negated = clux_context_prefix(ctx, CLUX_OP_LNOT, shadow, NULL);
  ASSERT_NE(negated, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(negated));
  EXPECT_EQ(clux_value_get_type(negated), clux_context_get_bool_type(ctx));

  clux_value_t *error = clux_context_create_error_message(ctx, NULL, "boom");
  clux_value_t *propagated =
      clux_context_prefix(ctx, CLUX_OP_LNOT, error, NULL);
  EXPECT_EQ(propagated, error);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BuiltinStrType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *type = clux_context_get_str_type(ctx);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(clux_type_get_kind(type), CLUX_TYPE_STR);
  EXPECT_STREQ(clux_type_get_name(type), "str");
  EXPECT_EQ(clux_type_get_size(type), sizeof(char *));
  EXPECT_EQ(clux_type_get_align(type), alignof(char *));
  clux_vtable_t *vt = clux_type_get_vtable(type);
  ASSERT_NE(vt, nullptr);
  EXPECT_NE(vt->add, nullptr);
  EXPECT_EQ(vt->sub, nullptr);
  EXPECT_EQ(vt->mul, nullptr);
  EXPECT_EQ(vt->div, nullptr);
  EXPECT_EQ(vt->mod, nullptr);
  EXPECT_EQ(vt->shl, nullptr);
  EXPECT_EQ(vt->shr, nullptr);
  EXPECT_EQ(vt->bit_and, nullptr);
  EXPECT_EQ(vt->bit_or, nullptr);
  EXPECT_EQ(vt->bit_xor, nullptr);
  EXPECT_EQ(vt->plus, nullptr);
  EXPECT_EQ(vt->negative, nullptr);
  EXPECT_EQ(vt->bit_not, nullptr);
  EXPECT_EQ(vt->logic_not, nullptr);
  EXPECT_NE(vt->eq, nullptr);
  EXPECT_NE(vt->ne, nullptr);
  EXPECT_EQ(vt->lt, nullptr);
  EXPECT_EQ(vt->le, nullptr);
  EXPECT_EQ(vt->gt, nullptr);
  EXPECT_EQ(vt->ge, nullptr);
  EXPECT_EQ(vt->safe_cast, nullptr);
  EXPECT_EQ(vt->cast, nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StrCreateAndConcat) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *hello = clux_context_create_str(ctx, "hello ", NULL, NULL);
  ASSERT_NE(hello, nullptr);
  EXPECT_EQ(clux_value_get_type(hello), clux_context_get_str_type(ctx));
  EXPECT_STREQ(read_str(hello), "hello ");

  clux_value_t *world = clux_context_create_str(ctx, "world", NULL, NULL);
  clux_value_t *joined = clux_context_add(ctx, hello, world, NULL);
  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(clux_value_get_type(joined), clux_context_get_str_type(ctx));
  EXPECT_STREQ(read_str(joined), "hello world");

  clux_value_t *bang = clux_context_create_str(ctx, "!", NULL, NULL);
  clux_value_t *chained = clux_context_add(ctx, joined, bang, NULL);
  ASSERT_NE(chained, nullptr);
  EXPECT_STREQ(read_str(chained), "hello world!");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StrConcatWithEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *empty = clux_context_create_str(ctx, "", NULL, NULL);
  clux_value_t *text = clux_context_create_str(ctx, "x", NULL, NULL);
  clux_value_t *right = clux_context_add(ctx, empty, text, NULL);
  ASSERT_NE(right, nullptr);
  EXPECT_STREQ(read_str(right), "x");
  clux_value_t *left = clux_context_add(ctx, text, empty, NULL);
  ASSERT_NE(left, nullptr);
  EXPECT_STREQ(read_str(left), "x");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StrAddRequiresStrOperands) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *text = clux_context_create_str(ctx, "x", NULL, NULL);
  clux_value_t *num = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *mixed = clux_context_add(ctx, text, num, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(mixed)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(mixed),
               "operator '+' requires operands of the same type");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StrShadowAndErrorPropagate) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_str_type(ctx));
  clux_value_t *text = clux_context_create_str(ctx, "x", NULL, NULL);
  clux_value_t *shadow_joined = clux_context_add(ctx, shadow, text, NULL);
  ASSERT_NE(shadow_joined, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_joined));
  EXPECT_EQ(clux_value_get_type(shadow_joined), clux_context_get_str_type(ctx));

  clux_value_t *error = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_add(ctx, error, text, NULL), error);
  EXPECT_EQ(clux_context_add(ctx, text, error, NULL), error);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StrUnsupportedOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *left = clux_context_create_str(ctx, "a", NULL, NULL);
  clux_value_t *right = clux_context_create_str(ctx, "b", NULL, NULL);
  clux_value_t *sub = clux_context_binary(ctx, CLUX_OP_SUB, left, right, NULL);
  ASSERT_NE(sub, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(sub)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(sub),
               "type 'str' does not support operator '-'");

  clux_value_t *safe =
      clux_context_safe_cast(ctx, left, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(safe, nullptr);
  EXPECT_STREQ(error_message_of(safe),
               "type 'str' cannot be safely cast to 'i32'");

  clux_value_t *cast =
      clux_context_cast(ctx, left, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(cast, nullptr);
  EXPECT_STREQ(error_message_of(cast), "type 'str' cannot be cast to 'i32'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, IntegerComparisonOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *small = clux_context_create_i8(ctx, 20, NULL, NULL);
  clux_value_t *large = clux_context_create_i32(ctx, 22, NULL, NULL);
  struct {
    clux_binary_op_t op;
    bool expected;
  } cases[] = {
      {CLUX_OP_EQ, false},
      {CLUX_OP_NE, true},
      {CLUX_OP_LT, true},
      {CLUX_OP_LE, true},
      {CLUX_OP_GT, false},
      {CLUX_OP_GE, false},
  };
  for (const auto &c : cases) {
    clux_value_t *result = clux_context_binary(ctx, c.op, small, large, NULL);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(clux_value_get_type(result), clux_context_get_bool_type(ctx));
    EXPECT_EQ(read_bool(result), c.expected);
  }

  clux_value_t *negative = clux_context_create_i8(ctx, -1, NULL, NULL);
  clux_value_t *positive = clux_context_create_i8(ctx, 1, NULL, NULL);
  clux_value_t *cmp =
      clux_context_binary(ctx, CLUX_OP_LT, negative, positive, NULL);
  ASSERT_NE(cmp, nullptr);
  EXPECT_TRUE(read_bool(cmp));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, UnsignedComparisonOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *small = clux_context_create_u8(ctx, 5, NULL, NULL);
  clux_value_t *large = clux_context_create_u32(ctx, 300, NULL, NULL);
  clux_value_t *cmp = clux_context_binary(ctx, CLUX_OP_LT, small, large, NULL);
  ASSERT_NE(cmp, nullptr);
  EXPECT_EQ(clux_value_get_type(cmp), clux_context_get_bool_type(ctx));
  EXPECT_TRUE(read_bool(cmp));

  clux_value_t *max = clux_context_create_u8(ctx, 255, NULL, NULL);
  clux_value_t *near_max = clux_context_create_u8(ctx, 254, NULL, NULL);
  clux_value_t *ge = clux_context_binary(ctx, CLUX_OP_GE, max, near_max, NULL);
  ASSERT_NE(ge, nullptr);
  EXPECT_TRUE(read_bool(ge));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatComparisonOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *small = clux_context_create_f64(ctx, 1.5, NULL, NULL);
  clux_value_t *large = clux_context_create_f64(ctx, 2.5, NULL, NULL);
  struct {
    clux_binary_op_t op;
    bool expected;
  } cases[] = {
      {CLUX_OP_EQ, false},
      {CLUX_OP_NE, true},
      {CLUX_OP_LT, true},
      {CLUX_OP_LE, true},
      {CLUX_OP_GT, false},
      {CLUX_OP_GE, false},
  };
  for (const auto &c : cases) {
    clux_value_t *result = clux_context_binary(ctx, c.op, small, large, NULL);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(clux_value_get_type(result), clux_context_get_bool_type(ctx));
    EXPECT_EQ(read_bool(result), c.expected);
  }

  clux_value_t *same_a = clux_context_create_f32(ctx, 2.5f, NULL, NULL);
  clux_value_t *same_b = clux_context_create_f64(ctx, 2.5, NULL, NULL);
  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, same_a, same_b, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_TRUE(read_bool(eq));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BoolComparisonOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *truth = clux_context_create_bool(ctx, true, NULL, NULL);
  clux_value_t *lie = clux_context_create_bool(ctx, false, NULL, NULL);
  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, truth, truth, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_EQ(clux_value_get_type(eq), clux_context_get_bool_type(ctx));
  EXPECT_TRUE(read_bool(eq));
  clux_value_t *ne = clux_context_binary(ctx, CLUX_OP_NE, truth, lie, NULL);
  ASSERT_NE(ne, nullptr);
  EXPECT_TRUE(read_bool(ne));

  clux_value_t *num = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *mixed = clux_context_binary(ctx, CLUX_OP_EQ, truth, num, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(mixed)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(mixed),
               "operator '==' requires operands of the same type");

  clux_value_t *ordered =
      clux_context_binary(ctx, CLUX_OP_LT, truth, lie, NULL);
  ASSERT_NE(ordered, nullptr);
  EXPECT_STREQ(error_message_of(ordered),
               "type 'bool' does not support operator '<'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StrComparisonOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *abc = clux_context_create_str(ctx, "abc", NULL, NULL);
  clux_value_t *abc2 = clux_context_create_str(ctx, "abc", NULL, NULL);
  clux_value_t *abd = clux_context_create_str(ctx, "abd", NULL, NULL);
  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, abc, abc2, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_EQ(clux_value_get_type(eq), clux_context_get_bool_type(ctx));
  EXPECT_TRUE(read_bool(eq));
  clux_value_t *ne = clux_context_binary(ctx, CLUX_OP_NE, abc, abd, NULL);
  ASSERT_NE(ne, nullptr);
  EXPECT_TRUE(read_bool(ne));

  clux_value_t *num = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *mixed = clux_context_binary(ctx, CLUX_OP_EQ, abc, num, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(mixed)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(mixed),
               "operator '==' requires operands of the same type");

  clux_value_t *ordered = clux_context_binary(ctx, CLUX_OP_LT, abc, abd, NULL);
  ASSERT_NE(ordered, nullptr);
  EXPECT_STREQ(error_message_of(ordered),
               "type 'str' does not support operator '<'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ComparisonShadowAndErrorPropagate) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *shadow_cmp =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow, value, NULL);
  ASSERT_NE(shadow_cmp, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_cmp));
  EXPECT_EQ(clux_value_get_type(shadow_cmp), clux_context_get_bool_type(ctx));

  clux_value_t *error = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_EQ, error, value, NULL), error);
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_LT, value, error, NULL), error);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PtrTypeIsInternedByBase) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32 = clux_context_get_i32_type(ctx);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32);
  ASSERT_NE(ptr_i32, nullptr);
  EXPECT_EQ(clux_context_get_ptr_type(ctx, i32), ptr_i32);
  EXPECT_STREQ(clux_type_get_name(ptr_i32), "*i32");
  EXPECT_EQ(clux_type_get_kind(ptr_i32), CLUX_TYPE_PTR);
  EXPECT_EQ(clux_type_get_size(ptr_i32), sizeof(void *));
  clux_vtable_t *vt = clux_type_get_vtable(ptr_i32);
  ASSERT_NE(vt, nullptr);
  EXPECT_EQ(vt, get_clux_ptr_vtable());
  EXPECT_NE(vt->eq, nullptr);
  EXPECT_NE(vt->ne, nullptr);
  EXPECT_NE(vt->deref_get, nullptr);
  EXPECT_NE(vt->deref_set, nullptr);
  EXPECT_EQ(vt->add, nullptr);
  EXPECT_EQ(vt->safe_cast, nullptr);
  EXPECT_EQ(vt->cast, nullptr);

  clux_type_t *ptr_f64 =
      clux_context_get_ptr_type(ctx, clux_context_get_f64_type(ctx));
  ASSERT_NE(ptr_f64, nullptr);
  EXPECT_NE(ptr_f64, ptr_i32);
  EXPECT_STREQ(clux_type_get_name(ptr_f64), "*f64");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, RefAndDerefRoundTrip) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *x = clux_context_create_i32(ctx, 42, "x", NULL);
  clux_value_t *p = clux_context_ref(ctx, x, NULL);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(clux_value_get_type(p),
            clux_context_get_ptr_type(ctx, clux_context_get_i32_type(ctx)));
  EXPECT_FALSE(clux_value_is_shadow(p));

  clux_value_t *read = clux_context_deref_get(ctx, p, NULL);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(clux_value_get_type(read), clux_context_get_i32_type(ctx));
  EXPECT_EQ(read_i32(read), 42);

  clux_value_t *n = clux_context_create_i32(ctx, 99, NULL, NULL);
  clux_value_t *assigned = clux_context_deref_set(ctx, p, n, NULL);
  EXPECT_EQ(assigned, n);
  clux_value_t *reread = clux_context_deref_get(ctx, p, NULL);
  ASSERT_NE(reread, nullptr);
  EXPECT_EQ(read_i32(reread), 99);
  /* 写穿透：目标值本体同步更新。 */
  EXPECT_EQ(read_i32(x), 99);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PtrToPtrType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *x = clux_context_create_i32(ctx, 7, NULL, NULL);
  clux_value_t *p = clux_context_ref(ctx, x, NULL);
  clux_value_t *pp = clux_context_ref(ctx, p, NULL);
  ASSERT_NE(pp, nullptr);
  clux_type_t *ptr_i32 =
      clux_context_get_ptr_type(ctx, clux_context_get_i32_type(ctx));
  clux_type_t *ptr_ptr_i32 = clux_context_get_ptr_type(ctx, ptr_i32);
  EXPECT_EQ(clux_value_get_type(p), ptr_i32);
  EXPECT_EQ(clux_value_get_type(pp), ptr_ptr_i32);
  EXPECT_STREQ(clux_type_get_name(ptr_ptr_i32), "**i32");
  EXPECT_EQ(clux_type_find_child(ptr_i32, CLUX_TYPE_PTR, NULL, NULL),
            ptr_ptr_i32);

  /* 解引用一层拿到指针副本，指向同一目标。 */
  clux_value_t *deref = clux_context_deref_get(ctx, pp, NULL);
  ASSERT_NE(deref, nullptr);
  EXPECT_EQ(clux_value_get_type(deref), ptr_i32);
  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, deref, p, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_TRUE(read_bool(eq));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, RefOfShadowAndErrorPropagate) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));
  clux_value_t *p_shadow = clux_context_ref(ctx, shadow, NULL);
  ASSERT_NE(p_shadow, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(p_shadow));
  EXPECT_STREQ(clux_type_get_name(clux_value_get_type(p_shadow)), "*i32");
  clux_value_t *shadow_read = clux_context_deref_get(ctx, p_shadow, NULL);
  ASSERT_NE(shadow_read, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_read));
  EXPECT_EQ(clux_value_get_type(shadow_read), clux_context_get_i32_type(ctx));

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *shadow_set = clux_context_deref_set(ctx, p_shadow, x, NULL);
  ASSERT_NE(shadow_set, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_set));
  EXPECT_EQ(clux_value_get_type(shadow_set), clux_context_get_i32_type(ctx));

  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_ref(ctx, err, NULL), err);
  EXPECT_EQ(clux_context_deref_get(ctx, err, NULL), err);
  EXPECT_EQ(clux_context_deref_set(ctx, err, x, NULL), err);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PtrComparisonOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *y = clux_context_create_i32(ctx, 2, NULL, NULL);
  clux_value_t *px = clux_context_ref(ctx, x, NULL);
  clux_value_t *px2 = clux_context_ref(ctx, x, NULL);
  clux_value_t *py = clux_context_ref(ctx, y, NULL);

  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, px, px2, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_EQ(clux_value_get_type(eq), clux_context_get_bool_type(ctx));
  EXPECT_TRUE(read_bool(eq));
  clux_value_t *ne = clux_context_binary(ctx, CLUX_OP_NE, px, py, NULL);
  ASSERT_NE(ne, nullptr);
  EXPECT_TRUE(read_bool(ne));
  clux_value_t *eq_diff = clux_context_binary(ctx, CLUX_OP_EQ, px, py, NULL);
  ASSERT_NE(eq_diff, nullptr);
  EXPECT_FALSE(read_bool(eq_diff));

  clux_value_t *mixed = clux_context_binary(ctx, CLUX_OP_EQ, px, x, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(mixed)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(mixed),
               "operator '==' requires operands of the same type");

  clux_value_t *shadow_px = clux_context_ref(
      ctx,
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx)),
      NULL);
  clux_value_t *shadow_cmp =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow_px, px, NULL);
  ASSERT_NE(shadow_cmp, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_cmp));
  EXPECT_EQ(clux_value_get_type(shadow_cmp), clux_context_get_bool_type(ctx));

  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_EQ, err, px, NULL), err);
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_NE, px, err, NULL), err);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PtrDerefSetRequiresBaseType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *p = clux_context_ref(ctx, x, NULL);
  clux_value_t *wide = clux_context_create_i64(ctx, 2, NULL, NULL);
  clux_value_t *mismatch = clux_context_deref_set(ctx, p, wide, NULL);
  ASSERT_NE(mismatch, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(mismatch)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(mismatch),
               "cannot assign 'i64' through a pointer to 'i32'");
  /* 失败的赋值不改变目标值。 */
  EXPECT_EQ(read_i32(x), 1);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PtrUnsupportedOps) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *p = clux_context_ref(ctx, x, NULL);

  clux_value_t *add = clux_context_binary(ctx, CLUX_OP_ADD, p, p, NULL);
  ASSERT_NE(add, nullptr);
  EXPECT_STREQ(error_message_of(add),
               "type '*i32' does not support operator '+'");
  clux_value_t *lt = clux_context_binary(ctx, CLUX_OP_LT, p, p, NULL);
  ASSERT_NE(lt, nullptr);
  EXPECT_STREQ(error_message_of(lt),
               "type '*i32' does not support operator '<'");
  clux_value_t *lnot = clux_context_prefix(ctx, CLUX_OP_LNOT, p, NULL);
  ASSERT_NE(lnot, nullptr);
  EXPECT_STREQ(error_message_of(lnot),
               "type '*i32' does not support operator '!'");

  clux_value_t *deref_of_value = clux_context_deref_get(ctx, x, NULL);
  ASSERT_NE(deref_of_value, nullptr);
  EXPECT_STREQ(error_message_of(deref_of_value),
               "type 'i32' does not support operator '*'");
  clux_value_t *deref_set_of_value = clux_context_deref_set(ctx, x, x, NULL);
  ASSERT_NE(deref_set_of_value, nullptr);
  EXPECT_STREQ(error_message_of(deref_set_of_value),
               "type 'i32' does not support operator '*'");

  clux_value_t *safe_cast =
      clux_context_safe_cast(ctx, p, clux_context_get_i32_type(ctx), NULL);
  ASSERT_NE(safe_cast, nullptr);
  EXPECT_STREQ(error_message_of(safe_cast),
               "type '*i32' cannot be safely cast to 'i32'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

static bool child_name_is(clux_type_t *child, void *user_data) {
  return strcmp(clux_type_get_name(child), (const char *)user_data) == 0;
}

TEST(EngineTest, ChildBucketsHoldMultipleSameKind) {
  allocator_t *a = create_allocator(malloc, free);
  clux_type_t *parent =
      create_clux_type(a, CLUX_TYPE_I32, "parent", 4, 4, NULL);
  clux_type_t *first = create_clux_type(a, CLUX_TYPE_PTR, "first", 8, 8, NULL);
  clux_type_t *second =
      create_clux_type(a, CLUX_TYPE_PTR, "second", 8, 8, NULL);
  clux_type_add_child(parent, CLUX_TYPE_PTR, first);
  clux_type_add_child(parent, CLUX_TYPE_PTR, second);

  EXPECT_EQ(clux_type_find_child(parent, CLUX_TYPE_I64, NULL, NULL), nullptr);
  /* pred 为 NULL 取桶首。 */
  EXPECT_EQ(clux_type_find_child(parent, CLUX_TYPE_PTR, NULL, NULL), first);
  /* 谓词在桶内按语义判别。 */
  EXPECT_EQ(clux_type_find_child(
                parent, CLUX_TYPE_PTR, child_name_is, (void *)"second"),
            second);
  EXPECT_EQ(clux_type_find_child(
                parent, CLUX_TYPE_PTR, child_name_is, (void *)"nope"),
            nullptr);
  /* 递归销毁带走桶内全部 child。 */
  delete_clux_type(&parent);
  delete_allocator(&a);
}

TEST(EngineTest, TypeEqualByKind) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_context_t *other_ctx = create_clux_context(a);

  clux_type_t *i32 = clux_context_get_i32_type(ctx);
  clux_type_t *i32_b = clux_context_get_i32_type(other_ctx);
  clux_type_t *i64 = clux_context_get_i64_type(ctx);
  clux_type_t *str = clux_context_get_str_type(ctx);
  /* 同指针与 kind 判等两个路径都为 true。 */
  EXPECT_TRUE(clux_type_equal(i32, i32));
  EXPECT_TRUE(clux_type_equal(i32, i32_b));
  EXPECT_FALSE(clux_type_equal(i32, i64));
  EXPECT_FALSE(clux_type_equal(str, i32));
  EXPECT_FALSE(clux_type_equal(i32, NULL));
  EXPECT_FALSE(clux_type_equal(NULL, i32));

  /* 无 vtable 的裸类型走指针判等 fallback。 */
  clux_type_t *bare1 = create_clux_type(a, CLUX_TYPE_I32, "bare", 4, 4, NULL);
  clux_type_t *bare2 = create_clux_type(a, CLUX_TYPE_I32, "bare", 4, 4, NULL);
  EXPECT_TRUE(clux_type_equal(bare1, bare1));
  EXPECT_FALSE(clux_type_equal(bare1, bare2));
  EXPECT_FALSE(clux_type_equal(bare1, i32));
  delete_clux_type(&bare1);
  delete_clux_type(&bare2);

  EXPECT_TRUE(clux_type_extends(i32, i32));
  EXPECT_TRUE(clux_type_extends(i32, i32_b));
  EXPECT_FALSE(clux_type_extends(i32, i64));

  delete_clux_context(&other_ctx);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PtrTypeEqualAndExtendsRecursive) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_context_t *other_ctx = create_clux_context(a);

  clux_type_t *i32 = clux_context_get_i32_type(ctx);
  clux_type_t *i32_b = clux_context_get_i32_type(other_ctx);
  clux_type_t *ptr_i32 = clux_type_get_ptr_type(i32_b);
  clux_type_t *ptr_i32_b = clux_type_get_ptr_type(i32);
  clux_type_t *ptr_i64 = clux_type_get_ptr_type(clux_context_get_i64_type(ctx));

  /* 递归判等：不同实例同 kind 同 base。 */
  EXPECT_TRUE(clux_type_equal(ptr_i32, ptr_i32_b));
  EXPECT_FALSE(clux_type_equal(ptr_i32, ptr_i64));
  EXPECT_FALSE(clux_type_equal(ptr_i32, i32));
  /* 协变 extends：*T extends *U 当且仅当 T extends U。 */
  EXPECT_TRUE(clux_type_extends(ptr_i32, ptr_i32_b));
  EXPECT_FALSE(clux_type_extends(ptr_i32, ptr_i64));
  EXPECT_FALSE(clux_type_extends(ptr_i32, i32));
  EXPECT_FALSE(clux_type_extends(i32, ptr_i32));

  /* 两层递归：**i32 vs **i32。 */
  clux_type_t *ptr_ptr_i32 = clux_type_get_ptr_type(ptr_i32);
  clux_type_t *ptr_ptr_i32_b = clux_type_get_ptr_type(ptr_i32_b);
  EXPECT_TRUE(clux_type_equal(ptr_ptr_i32, ptr_ptr_i32_b));
  EXPECT_FALSE(clux_type_equal(ptr_ptr_i32, ptr_i32));
  EXPECT_TRUE(clux_type_extends(ptr_ptr_i32, ptr_ptr_i32_b));
  EXPECT_FALSE(clux_type_extends(ptr_ptr_i32, ptr_i64));

  /* 桶去重判据：kind 判等的不同 base 实例，其 ptr 类型语义相等，
     find-child 谓词据此命中。 */
  clux_type_t *fake_i32 =
      create_clux_type(a, CLUX_TYPE_I32, "i32", 4, 4, get_clux_i32_vtable());
  clux_type_t *fake_ptr_i32 = clux_type_get_ptr_type(fake_i32);
  EXPECT_TRUE(clux_type_equal(fake_ptr_i32, ptr_i32));
  EXPECT_TRUE(clux_type_extends(fake_ptr_i32, ptr_i32_b));
  delete_clux_type(&fake_i32);

  delete_clux_context(&other_ctx);
  delete_clux_context(&ctx);
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

TEST(EngineTest, AssignSafeCastsRightToLeftType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  /* 右值 safe_cast 成左值类型后 memcpy 穿透，返回左值本身 */
  clux_value_t *target = clux_context_create_i32(ctx, 0, "x", NULL);
  clux_value_t *right = clux_context_create_i8(ctx, 42, NULL, NULL);
  clux_value_t *result = clux_context_assign(ctx, target, right, NULL);
  EXPECT_EQ(result, target);
  EXPECT_EQ(read_i32(target), 42);

  /* 同类型恒等路径 */
  clux_value_t *other = clux_context_create_i32(ctx, 7, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, target, other, NULL), target);
  EXPECT_EQ(read_i32(target), 7);

  /* 非整数目标拒绝赋值 */
  clux_value_t *bool_target = clux_context_create_bool(ctx, false, NULL, NULL);
  clux_value_t *bad = clux_context_assign(ctx, bool_target, other, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "cannot safely cast 'i32' to 'bool'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, AssignNarrowingProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *target = clux_context_create_i32(ctx, 0, NULL, NULL);
  clux_value_t *right = clux_context_create_i64(ctx, 123456789012, NULL, NULL);
  clux_value_t *bad = clux_context_assign(ctx, target, right, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "cannot safely cast 'i64' to 'i32': narrowing conversion");
  /* 左值保持原值 */
  EXPECT_EQ(read_i32(target), 0);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, AssignPropagatesErrorShadowAndNull) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  /* error 自传播：右值 */
  clux_value_t *target = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *err_right =
      clux_context_assign(ctx, target, NULL, NULL); /* 先拿一个 error */
  ASSERT_NE(err_right, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(err_right)),
            CLUX_TYPE_ERROR);
  clux_value_t *propagated = clux_context_assign(ctx, target, err_right, NULL);
  EXPECT_EQ(propagated, err_right);

  /* error 自传播：左值 */
  clux_value_t *value = clux_context_create_i32(ctx, 2, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, err_right, value, NULL), err_right);

  /* shadow 左值：返回左值 shadow 本身，不发生写入 */
  clux_value_t *shadow_target =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));
  clux_value_t *shadow_result =
      clux_context_assign(ctx, shadow_target, value, NULL);
  EXPECT_EQ(shadow_result, shadow_target);
  EXPECT_TRUE(clux_value_is_shadow(shadow_target));

  /* 右值 shadow：返回左值，数据不变 */
  clux_value_t *shadow_right =
      clux_context_create_shadow_value(ctx, clux_context_get_i32_type(ctx));
  clux_value_t *mixed = clux_context_assign(ctx, target, shadow_right, NULL);
  EXPECT_EQ(mixed, target);
  EXPECT_EQ(read_i32(target), 1);

  /* NULL 入口 */
  clux_value_t *bad = clux_context_assign(ctx, NULL, value, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "assignment requires a target and a value");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CloneCopiesTrivialData) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  /* 数值克隆：同类型、同值、数据块独立 */
  clux_value_t *orig = clux_context_create_i32(ctx, 42, NULL, NULL);
  clux_value_t *copy = clux_context_clone(ctx, orig, NULL);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy, orig);
  EXPECT_EQ(clux_value_get_type(copy), clux_value_get_type(orig));
  EXPECT_EQ(read_i32(copy), 42);
  EXPECT_NE(clux_value_get_data(copy), clux_value_get_data(orig));

  /* 改写原值不影响克隆 */
  clux_value_t *other = clux_context_create_i32(ctx, 100, NULL, NULL);
  ASSERT_NE(clux_context_assign(ctx, orig, other, NULL), nullptr);
  EXPECT_EQ(read_i32(copy), 42);

  /* str 克隆：借用指针平凡拷贝，内容相等 */
  clux_value_t *text = clux_context_create_str(ctx, "hello", NULL, NULL);
  clux_value_t *text_copy = clux_context_clone(ctx, text, NULL);
  ASSERT_NE(text_copy, nullptr);
  EXPECT_EQ(clux_value_get_type(text_copy), clux_value_get_type(text));
  EXPECT_STREQ(read_str(text_copy), "hello");

  /* ptr 克隆：handle 平凡拷贝，判等成立 */
  clux_value_t *ptr = clux_context_ref(ctx, orig, NULL);
  clux_value_t *ptr_copy = clux_context_clone(ctx, ptr, NULL);
  ASSERT_NE(ptr_copy, nullptr);
  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, ptr, ptr_copy, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_EQ(read_bool(eq), true);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ClonePropagatesErrorShadowAndNull) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  /* error 自传播：克隆 error 返回自身 */
  clux_value_t *bad = clux_context_clone(ctx, NULL, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_EQ(clux_context_clone(ctx, bad, NULL), bad);

  /* shadow 克隆为同类型 shadow */
  clux_value_t *shadow =
      clux_context_create_shadow_value(ctx, clux_context_get_i64_type(ctx));
  clux_value_t *shadow_copy = clux_context_clone(ctx, shadow, NULL);
  ASSERT_NE(shadow_copy, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_copy));
  EXPECT_EQ(clux_value_get_type(shadow_copy), clux_context_get_i64_type(ctx));

  delete_clux_context(&ctx);
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
  clux_value_t *text = clux_context_create_str(ctx, "str", NULL, NULL);
  clux_value_t *joined = clux_context_add(ctx, text, text, NULL);
  ASSERT_NE(joined, nullptr);
  clux_value_t *ptr = clux_context_ref(ctx, left, NULL);
  ASSERT_NE(ptr, nullptr);
  clux_value_t *deref = clux_context_deref_get(ctx, ptr, NULL);
  ASSERT_NE(deref, nullptr);
  EXPECT_EQ(clux_context_deref_set(ctx, ptr, right, NULL), right);
  clux_value_t *same = clux_context_binary(ctx, CLUX_OP_EQ, ptr, ptr, NULL);
  ASSERT_NE(same, nullptr);
  clux_value_t *ptr_shadow = clux_context_ref(
      ctx,
      clux_context_create_shadow_value(ctx, clux_context_get_str_type(ctx)),
      NULL);
  ASSERT_NE(ptr_shadow, nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
