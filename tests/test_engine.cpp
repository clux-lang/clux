#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/safe_alloc.h"
#include "engine/array.h"
#include "engine/context.h"
#include "engine/func.h"
#include "engine/integer.h"
#include "engine/interface.h"
#include "engine/optional.h"
#include "engine/ptr.h"
#include "engine/qualifier.h"
#include "engine/slice.h"
#include "engine/type.h"
#include "engine/template.h"
#include "engine/type_value.h"
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

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
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
      {clux_context_builtin_type(ctx, CLUX_TYPE_I8),
       CLUX_TYPE_I8,
       "i8",
       sizeof(int8_t)},
      {clux_context_builtin_type(ctx, CLUX_TYPE_I16),
       CLUX_TYPE_I16,
       "i16",
       sizeof(int16_t)},
      {clux_context_builtin_type(ctx, CLUX_TYPE_I32),
       CLUX_TYPE_I32,
       "i32",
       sizeof(int32_t)},
      {clux_context_builtin_type(ctx, CLUX_TYPE_I64),
       CLUX_TYPE_I64,
       "i64",
       sizeof(int64_t)},
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
  EXPECT_EQ(clux_value_get_type(i8_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_I8));
  EXPECT_EQ(read_i8(i8_sum), 42);

  clux_value_t *i16_left = clux_context_create_i16(ctx, 200, NULL, NULL);
  clux_value_t *i16_right = clux_context_create_i16(ctx, 300, NULL, NULL);
  clux_value_t *i16_sum = clux_context_add(ctx, i16_left, i16_right, NULL);
  ASSERT_NE(i16_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i16_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_I16));
  EXPECT_EQ(read_i16(i16_sum), 500);

  clux_value_t *i64_left =
      clux_context_create_i64(ctx, INT64_C(4000000000), NULL, NULL);
  clux_value_t *i64_right =
      clux_context_create_i64(ctx, INT64_C(2000000000), NULL, NULL);
  clux_value_t *i64_sum = clux_context_add(ctx, i64_left, i64_right, NULL);
  ASSERT_NE(i64_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i64_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(i64_sum), INT64_C(6000000000));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PlatformIsizeAndUsizeTypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  /* 元信息：宽度跟随平台指针宽度 */
  clux_type_t *isize_type = clux_context_builtin_type(ctx, CLUX_TYPE_ISIZE);
  clux_type_t *usize_type = clux_context_builtin_type(ctx, CLUX_TYPE_USIZE);
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
  EXPECT_EQ(clux_context_builtin_type(ctx, CLUX_TYPE_ISIZE), isize_type);
  EXPECT_EQ(clux_context_builtin_type(ctx, CLUX_TYPE_USIZE), usize_type);

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
      ctx, isize_left, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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

  clux_value_t *usize_to_u32 = clux_context_cast(
      ctx, usize_left, clux_context_builtin_type(ctx, CLUX_TYPE_U32), NULL);
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
  EXPECT_EQ(clux_value_get_type(i16_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_I16));
  EXPECT_EQ(read_i16(i16_sum), 320);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 100, NULL, NULL);
  clux_value_t *i64_value =
      clux_context_create_i64(ctx, INT64_C(4000000000), NULL, NULL);
  clux_value_t *i64_sum =
      clux_context_binary(ctx, CLUX_OP_ADD, i32_value, i64_value, NULL);
  ASSERT_NE(i64_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i64_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(i64_sum), INT64_C(4000000100));

  clux_value_t *i64_left =
      clux_context_create_i64(ctx, INT64_C(4000000000), NULL, NULL);
  clux_value_t *i32_right = clux_context_create_i32(ctx, 100, NULL, NULL);
  clux_value_t *i64_right_sum =
      clux_context_binary(ctx, CLUX_OP_ADD, i64_left, i32_right, NULL);
  ASSERT_NE(i64_right_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(i64_right_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(i64_right_sum), INT64_C(4000000100));

  clux_value_t *shift_count = clux_context_create_i16(ctx, 2, NULL, NULL);
  clux_value_t *shifted =
      clux_context_binary(ctx, CLUX_OP_SHL, i8_value, shift_count, NULL);
  ASSERT_NE(shifted, nullptr);
  EXPECT_EQ(clux_value_get_type(shifted),
            clux_context_builtin_type(ctx, CLUX_TYPE_I16));
  EXPECT_EQ(read_i16(shifted), 80);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SafeCastWidensIntegers) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *i8_value = clux_context_create_i8(ctx, -5, NULL, NULL);
  clux_value_t *as_i16 = clux_context_safe_cast(
      ctx, i8_value, clux_context_builtin_type(ctx, CLUX_TYPE_I16), NULL);
  ASSERT_NE(as_i16, nullptr);
  EXPECT_EQ(clux_value_get_type(as_i16),
            clux_context_builtin_type(ctx, CLUX_TYPE_I16));
  EXPECT_EQ(read_i16(as_i16), -5);

  clux_value_t *as_i32 = clux_context_safe_cast(
      ctx, i8_value, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
  ASSERT_NE(as_i32, nullptr);
  EXPECT_EQ(clux_value_get_type(as_i32),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(as_i32), -5);

  clux_value_t *as_i64 = clux_context_safe_cast(
      ctx, i8_value, clux_context_builtin_type(ctx, CLUX_TYPE_I64), NULL);
  ASSERT_NE(as_i64, nullptr);
  EXPECT_EQ(clux_value_get_type(as_i64),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(as_i64), -5);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 123456, NULL, NULL);
  clux_value_t *i32_as_i64 = clux_context_safe_cast(
      ctx, i32_value, clux_context_builtin_type(ctx, CLUX_TYPE_I64), NULL);
  ASSERT_NE(i32_as_i64, nullptr);
  EXPECT_EQ(clux_value_get_type(i32_as_i64),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(i32_as_i64), 123456);

  clux_value_t *identity = clux_context_safe_cast(
      ctx, i32_value, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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
       clux_context_builtin_type(ctx, CLUX_TYPE_I8)},
      {clux_context_create_i32(ctx, 1, NULL, NULL),
       clux_context_builtin_type(ctx, CLUX_TYPE_I16)},
      {clux_context_create_i64(ctx, 1, NULL, NULL),
       clux_context_builtin_type(ctx, CLUX_TYPE_I32)},
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
  clux_value_t *result = clux_context_safe_cast(
      ctx, value, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
  EXPECT_EQ(result, value);
  EXPECT_NE(strstr(error_message_of(result), "boom"), nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SafeCastNullInputsProduceError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);

  clux_value_t *no_value = clux_context_safe_cast(
      ctx, NULL, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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
    EXPECT_EQ(clux_value_get_type(result),
              clux_context_builtin_type(ctx, CLUX_TYPE_I32));
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
  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  ASSERT_NE(shadow, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow));
  EXPECT_EQ(clux_value_get_data(shadow), 0);
  EXPECT_EQ(clux_value_get_type(shadow),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));

  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);
  EXPECT_FALSE(clux_value_is_shadow(value));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BinaryWithShadowProducesShadow) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *value = clux_context_create_i32(ctx, 5, NULL, NULL);
  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));

  clux_value_t *left_shadow =
      clux_context_binary(ctx, CLUX_OP_ADD, shadow, value, NULL);
  ASSERT_NE(left_shadow, nullptr);
  EXPECT_EQ(clux_value_get_type(left_shadow),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_TRUE(clux_value_is_shadow(left_shadow));

  clux_value_t *right_shadow =
      clux_context_binary(ctx, CLUX_OP_MUL, value, shadow, NULL);
  ASSERT_NE(right_shadow, nullptr);
  EXPECT_EQ(clux_value_get_type(right_shadow),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_TRUE(clux_value_is_shadow(right_shadow));

  clux_value_t *both_shadow =
      clux_context_binary(ctx, CLUX_OP_SUB, shadow, shadow, NULL);
  ASSERT_NE(both_shadow, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(both_shadow));

  clux_value_t *i8_shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I8));
  clux_value_t *i16_value = clux_context_create_i16(ctx, 2, NULL, NULL);
  clux_value_t *widened =
      clux_context_binary(ctx, CLUX_OP_ADD, i8_shadow, i16_value, NULL);
  ASSERT_NE(widened, nullptr);
  EXPECT_EQ(clux_value_get_type(widened),
            clux_context_builtin_type(ctx, CLUX_TYPE_I16));
  EXPECT_TRUE(clux_value_is_shadow(widened));

  clux_value_t *i8_value = clux_context_create_i8(ctx, 2, NULL, NULL);
  clux_value_t *i16_shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I16));
  clux_value_t *widened_right =
      clux_context_binary(ctx, CLUX_OP_ADD, i8_value, i16_shadow, NULL);
  ASSERT_NE(widened_right, nullptr);
  EXPECT_EQ(clux_value_get_type(widened_right),
            clux_context_builtin_type(ctx, CLUX_TYPE_I16));
  EXPECT_TRUE(clux_value_is_shadow(widened_right));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PrefixWithShadowProducesShadow) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));

  clux_value_t *negative =
      clux_context_prefix(ctx, CLUX_OP_NEGATIVE, shadow, NULL);
  ASSERT_NE(negative, nullptr);
  EXPECT_EQ(clux_value_get_type(negative),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
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
  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I8));

  clux_value_t *widened = clux_context_safe_cast(
      ctx, shadow, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
  ASSERT_NE(widened, nullptr);
  EXPECT_EQ(clux_value_get_type(widened),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_TRUE(clux_value_is_shadow(widened));

  clux_value_t *i32_shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *narrowed = clux_context_safe_cast(
      ctx, i32_shadow, clux_context_builtin_type(ctx, CLUX_TYPE_I8), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_value_get_type(narrowed), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(narrowed), "narrowing conversion"),
            nullptr);

  clux_value_t *identity = clux_context_safe_cast(
      ctx, i32_shadow, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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
      {clux_context_builtin_type(ctx, CLUX_TYPE_U8),
       CLUX_TYPE_U8,
       "u8",
       sizeof(uint8_t)},
      {clux_context_builtin_type(ctx, CLUX_TYPE_U16),
       CLUX_TYPE_U16,
       "u16",
       sizeof(uint16_t)},
      {clux_context_builtin_type(ctx, CLUX_TYPE_U32),
       CLUX_TYPE_U32,
       "u32",
       sizeof(uint32_t)},
      {clux_context_builtin_type(ctx, CLUX_TYPE_U64),
       CLUX_TYPE_U64,
       "u64",
       sizeof(uint64_t)},
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
  EXPECT_EQ(clux_value_get_type(u8_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  EXPECT_EQ(read_u8(u8_sum), 255);

  clux_value_t *u16_left = clux_context_create_u16(ctx, 60000, NULL, NULL);
  clux_value_t *u16_right = clux_context_create_u16(ctx, 5000, NULL, NULL);
  clux_value_t *u16_sum = clux_context_add(ctx, u16_left, u16_right, NULL);
  ASSERT_NE(u16_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u16_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_U16));
  EXPECT_EQ(read_u16(u16_sum), 65000);

  clux_value_t *u32_left =
      clux_context_create_u32(ctx, UINT32_C(4000000000), NULL, NULL);
  clux_value_t *u32_right = clux_context_create_u32(ctx, 200, NULL, NULL);
  clux_value_t *u32_sum = clux_context_add(ctx, u32_left, u32_right, NULL);
  ASSERT_NE(u32_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u32_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_U32));
  EXPECT_EQ(read_u32(u32_sum), UINT32_C(4000000200));

  clux_value_t *u64_left =
      clux_context_create_u64(ctx, UINT64_C(10000000000), NULL, NULL);
  clux_value_t *u64_right =
      clux_context_create_u64(ctx, UINT64_C(20000000000), NULL, NULL);
  clux_value_t *u64_sum = clux_context_add(ctx, u64_left, u64_right, NULL);
  ASSERT_NE(u64_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u64_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_U64));
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
  EXPECT_EQ(clux_value_get_type(u16_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_U16));
  EXPECT_EQ(read_u16(u16_sum), 320);

  clux_value_t *u32_value = clux_context_create_u32(ctx, 100, NULL, NULL);
  clux_value_t *u64_value =
      clux_context_create_u64(ctx, UINT64_C(4000000000), NULL, NULL);
  clux_value_t *u64_sum =
      clux_context_binary(ctx, CLUX_OP_ADD, u32_value, u64_value, NULL);
  ASSERT_NE(u64_sum, nullptr);
  EXPECT_EQ(clux_value_get_type(u64_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_U64));
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
      ctx, u8_value, clux_context_builtin_type(ctx, CLUX_TYPE_U32), NULL);
  ASSERT_NE(widened, nullptr);
  EXPECT_EQ(clux_value_get_type(widened),
            clux_context_builtin_type(ctx, CLUX_TYPE_U32));
  EXPECT_EQ(read_u32(widened), 200);

  clux_value_t *narrowed = clux_context_safe_cast(
      ctx, widened, clux_context_builtin_type(ctx, CLUX_TYPE_U8), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_value_get_type(narrowed), clux_context_get_error_type(ctx));
  EXPECT_NE(strstr(error_message_of(narrowed), "narrowing conversion"),
            nullptr);

  clux_value_t *to_signed = clux_context_safe_cast(
      ctx, u8_value, clux_context_builtin_type(ctx, CLUX_TYPE_I64), NULL);
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
  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  clux_value_t *value = clux_context_create_u16(ctx, 2, NULL, NULL);
  clux_value_t *result =
      clux_context_binary(ctx, CLUX_OP_ADD, shadow, value, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_U16));
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
  clux_value_t *as_i8 = clux_context_cast(
      ctx, i32_value, clux_context_builtin_type(ctx, CLUX_TYPE_I8), NULL);
  ASSERT_NE(as_i8, nullptr);
  EXPECT_EQ(clux_value_get_type(as_i8),
            clux_context_builtin_type(ctx, CLUX_TYPE_I8));
  EXPECT_EQ(read_i8(as_i8), 42);

  clux_value_t *minus_one = clux_context_create_i32(ctx, -1, NULL, NULL);
  clux_value_t *as_u8 = clux_context_cast(
      ctx, minus_one, clux_context_builtin_type(ctx, CLUX_TYPE_U8), NULL);
  ASSERT_NE(as_u8, nullptr);
  EXPECT_EQ(clux_value_get_type(as_u8),
            clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  EXPECT_EQ(read_u8(as_u8), 255);

  clux_value_t *as_u32 = clux_context_cast(
      ctx, minus_one, clux_context_builtin_type(ctx, CLUX_TYPE_U32), NULL);
  ASSERT_NE(as_u32, nullptr);
  EXPECT_EQ(clux_value_get_type(as_u32),
            clux_context_builtin_type(ctx, CLUX_TYPE_U32));
  EXPECT_EQ(read_u32(as_u32), UINT32_MAX);

  clux_value_t *u8_value = clux_context_create_u8(ctx, 255, NULL, NULL);
  clux_value_t *as_i8_from_u8 = clux_context_cast(
      ctx, u8_value, clux_context_builtin_type(ctx, CLUX_TYPE_I8), NULL);
  ASSERT_NE(as_i8_from_u8, nullptr);
  EXPECT_EQ(read_i8(as_i8_from_u8), -1);

  clux_value_t *u32_value =
      clux_context_create_u32(ctx, UINT32_C(300), NULL, NULL);
  clux_value_t *as_u8_from_u32 = clux_context_cast(
      ctx, u32_value, clux_context_builtin_type(ctx, CLUX_TYPE_U8), NULL);
  ASSERT_NE(as_u8_from_u32, nullptr);
  EXPECT_EQ(read_u8(as_u8_from_u32), 44);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CastShadowPropagates) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I8));
  clux_value_t *result = clux_context_cast(
      ctx, shadow, clux_context_builtin_type(ctx, CLUX_TYPE_U64), NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_U64));
  EXPECT_TRUE(clux_value_is_shadow(result));
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CastPropagatesErrorValue) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_error_t *error = create_error(a, NULL, "boom");
  clux_value_t *value = clux_context_create_error(ctx, error);
  clux_value_t *result = clux_context_cast(
      ctx, value, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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

  clux_value_t *no_value = clux_context_cast(
      ctx, NULL, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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
  clux_value_t *result = clux_context_cast(
      ctx, value, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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
      {clux_context_builtin_type(ctx, CLUX_TYPE_F32),
       CLUX_TYPE_F32,
       "f32",
       sizeof(float)},
      {clux_context_builtin_type(ctx, CLUX_TYPE_F64),
       CLUX_TYPE_F64,
       "f64",
       sizeof(double)},
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
  EXPECT_EQ(clux_value_get_type(f32_sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_F32));
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
    EXPECT_EQ(clux_value_get_type(result),
              clux_context_builtin_type(ctx, CLUX_TYPE_F64));
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
  EXPECT_EQ(clux_value_get_type(sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_F64));
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
  EXPECT_EQ(clux_value_get_type(quotient),
            clux_context_builtin_type(ctx, CLUX_TYPE_F64));
  EXPECT_TRUE(std::isinf(read_f64(quotient)));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatSafeCastWidensOnly) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *f32_value = clux_context_create_f32(ctx, 1.5f, NULL, NULL);
  clux_value_t *widened = clux_context_safe_cast(
      ctx, f32_value, clux_context_builtin_type(ctx, CLUX_TYPE_F64), NULL);
  ASSERT_NE(widened, nullptr);
  EXPECT_EQ(clux_value_get_type(widened),
            clux_context_builtin_type(ctx, CLUX_TYPE_F64));
  EXPECT_DOUBLE_EQ(read_f64(widened), 1.5);

  clux_value_t *f64_value = clux_context_create_f64(ctx, 1.5, NULL, NULL);
  clux_value_t *narrowed = clux_context_safe_cast(
      ctx, f64_value, clux_context_builtin_type(ctx, CLUX_TYPE_F32), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(narrowed)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(narrowed),
               "cannot safely cast 'f64' to 'f32': narrowing conversion");

  clux_value_t *to_int = clux_context_safe_cast(
      ctx, f64_value, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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
  clux_value_t *to_i32 = clux_context_cast(
      ctx, f64_value, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
  ASSERT_NE(to_i32, nullptr);
  EXPECT_EQ(clux_value_get_type(to_i32),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(to_i32), 2);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 7, NULL, NULL);
  clux_value_t *to_f64 = clux_context_cast(
      ctx, i32_value, clux_context_builtin_type(ctx, CLUX_TYPE_F64), NULL);
  ASSERT_NE(to_f64, nullptr);
  EXPECT_EQ(clux_value_get_type(to_f64),
            clux_context_builtin_type(ctx, CLUX_TYPE_F64));
  EXPECT_DOUBLE_EQ(read_f64(to_f64), 7.0);

  clux_value_t *u8_value = clux_context_create_u8(ctx, 255, NULL, NULL);
  clux_value_t *u8_to_f32 = clux_context_cast(
      ctx, u8_value, clux_context_builtin_type(ctx, CLUX_TYPE_F32), NULL);
  ASSERT_NE(u8_to_f32, nullptr);
  EXPECT_EQ(clux_value_get_type(u8_to_f32),
            clux_context_builtin_type(ctx, CLUX_TYPE_F32));
  EXPECT_FLOAT_EQ(read_f32(u8_to_f32), 255.0f);

  clux_value_t *narrowed = clux_context_cast(
      ctx, f64_value, clux_context_builtin_type(ctx, CLUX_TYPE_F32), NULL);
  ASSERT_NE(narrowed, nullptr);
  EXPECT_EQ(clux_value_get_type(narrowed),
            clux_context_builtin_type(ctx, CLUX_TYPE_F32));
  EXPECT_FLOAT_EQ(read_f32(narrowed), 2.5f);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FloatShadowAndErrorPropagate) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *shadow_f32 = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_F32));
  clux_value_t *f64_value = clux_context_create_f64(ctx, 1.0, NULL, NULL);
  clux_value_t *sum = clux_context_add(ctx, shadow_f32, f64_value, NULL);
  ASSERT_NE(sum, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(sum));
  EXPECT_EQ(clux_value_get_type(sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_F64));

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

  clux_type_t *type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
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
  EXPECT_EQ(clux_value_get_type(negated),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
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
  clux_value_t *to_i32 = clux_context_cast(
      ctx, truth, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
  ASSERT_NE(to_i32, nullptr);
  EXPECT_EQ(read_i32(to_i32), 1);

  clux_value_t *lie = clux_context_create_bool(ctx, false, NULL, NULL);
  clux_value_t *to_u8 = clux_context_cast(
      ctx, lie, clux_context_builtin_type(ctx, CLUX_TYPE_U8), NULL);
  ASSERT_NE(to_u8, nullptr);
  EXPECT_EQ(read_u8(to_u8), 0);

  clux_value_t *to_f64 = clux_context_cast(
      ctx, truth, clux_context_builtin_type(ctx, CLUX_TYPE_F64), NULL);
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
    clux_value_t *result = clux_context_cast(
        ctx, c.value, clux_context_builtin_type(ctx, CLUX_TYPE_BOOL), NULL);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(clux_value_get_type(result),
              clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
    EXPECT_EQ(read_bool(result), c.expected);
  }

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, BoolSafeCastStrict) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *truth = clux_context_create_bool(ctx, true, NULL, NULL);
  clux_value_t *identity = clux_context_safe_cast(
      ctx, truth, clux_context_builtin_type(ctx, CLUX_TYPE_BOOL), NULL);
  EXPECT_EQ(identity, truth);

  clux_value_t *to_i32 = clux_context_safe_cast(
      ctx, truth, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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

  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  clux_value_t *negated = clux_context_prefix(ctx, CLUX_OP_LNOT, shadow, NULL);
  ASSERT_NE(negated, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(negated));
  EXPECT_EQ(clux_value_get_type(negated),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));

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

  clux_type_t *type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
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
  EXPECT_EQ(clux_value_get_type(hello),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  EXPECT_STREQ(read_str(hello), "hello ");

  clux_value_t *world = clux_context_create_str(ctx, "world", NULL, NULL);
  clux_value_t *joined = clux_context_add(ctx, hello, world, NULL);
  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(clux_value_get_type(joined),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));
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

  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  clux_value_t *text = clux_context_create_str(ctx, "x", NULL, NULL);
  clux_value_t *shadow_joined = clux_context_add(ctx, shadow, text, NULL);
  ASSERT_NE(shadow_joined, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_joined));
  EXPECT_EQ(clux_value_get_type(shadow_joined),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));

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

  clux_value_t *safe = clux_context_safe_cast(
      ctx, left, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
  ASSERT_NE(safe, nullptr);
  EXPECT_STREQ(error_message_of(safe),
               "type 'str' cannot be safely cast to 'i32'");

  clux_value_t *cast = clux_context_cast(
      ctx, left, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
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
    EXPECT_EQ(clux_value_get_type(result),
              clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
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
  EXPECT_EQ(clux_value_get_type(cmp),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
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
    EXPECT_EQ(clux_value_get_type(result),
              clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
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
  EXPECT_EQ(clux_value_get_type(eq),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
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
  EXPECT_EQ(clux_value_get_type(eq),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
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

  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *shadow_cmp =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow, value, NULL);
  ASSERT_NE(shadow_cmp, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_cmp));
  EXPECT_EQ(clux_value_get_type(shadow_cmp),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));

  clux_value_t *error = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_EQ, error, value, NULL), error);
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_LT, value, error, NULL), error);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PtrTypeIsInternedByBase) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32 = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
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
  EXPECT_NE(vt->deref, nullptr);
  EXPECT_EQ(vt->index, nullptr);
  EXPECT_EQ(vt->add, nullptr);
  EXPECT_NE(vt->safe_cast, nullptr);
  EXPECT_EQ(vt->cast, nullptr);

  clux_type_t *ptr_f64 = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_F64));
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
            clux_context_get_ptr_type(
                ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32)));
  EXPECT_FALSE(clux_value_is_shadow(p));

  /* 解引用：deref 返回 base 类型的 view（与目标内存共享） */
  clux_value_t *read = clux_context_deref(ctx, p, NULL);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(clux_value_get_type(read),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(read), 42);

  /* 写入走通用 assign：写穿 view 即写穿透到目标值本体 */
  clux_value_t *n = clux_context_create_i32(ctx, 99, NULL, NULL);
  clux_value_t *assigned = clux_context_assign(ctx, read, n, NULL);
  EXPECT_EQ(assigned, read);
  clux_value_t *reread = clux_context_deref(ctx, p, NULL);
  ASSERT_NE(reread, nullptr);
  EXPECT_EQ(read_i32(reread), 99);
  EXPECT_EQ(read_i32(x), 99);

  /* view 与底层内存别名：读穿 view 可见本体变化 */
  EXPECT_EQ(read_i32(read), 99);

  /* 赋值给具名变量经 memcpy 自动失去引用属性：后续写入不回写本体 */
  clux_value_t *named = clux_context_create_i32(ctx, 0, "named", NULL);
  EXPECT_EQ(clux_context_assign(ctx, named, reread, NULL), named);
  EXPECT_EQ(read_i32(named), 99);
  clux_value_t *m = clux_context_create_i32(ctx, 7, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, named, m, NULL), named);
  EXPECT_EQ(read_i32(named), 7);
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
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_type_t *ptr_ptr_i32 = clux_context_get_ptr_type(ctx, ptr_i32);
  EXPECT_EQ(clux_value_get_type(p), ptr_i32);
  EXPECT_EQ(clux_value_get_type(pp), ptr_ptr_i32);
  EXPECT_STREQ(clux_type_get_name(ptr_ptr_i32), "**i32");
  EXPECT_EQ(clux_type_find_child(ptr_i32, CLUX_TYPE_PTR, NULL, NULL),
            ptr_ptr_i32);

  /* 解引用一层拿到 *i32 的 view，指向同一目标。 */
  clux_value_t *deref = clux_context_deref(ctx, pp, NULL);
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

  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *p_shadow = clux_context_ref(ctx, shadow, NULL);
  ASSERT_NE(p_shadow, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(p_shadow));
  EXPECT_STREQ(clux_type_get_name(clux_value_get_type(p_shadow)), "*i32");
  clux_value_t *shadow_read = clux_context_deref(ctx, p_shadow, NULL);
  ASSERT_NE(shadow_read, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_read));
  EXPECT_EQ(clux_value_get_type(shadow_read),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *shadow_set = clux_context_assign(ctx, shadow_read, x, NULL);
  ASSERT_NE(shadow_set, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_set));
  EXPECT_EQ(clux_value_get_type(shadow_set),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));

  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_ref(ctx, err, NULL), err);
  EXPECT_EQ(clux_context_deref(ctx, err, NULL), err);
  EXPECT_EQ(clux_context_assign(ctx, err, x, NULL), err);

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
  EXPECT_EQ(clux_value_get_type(eq),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
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

  clux_value_t *shadow_px =
      clux_context_ref(ctx,
                       clux_context_create_shadow_value(
                           ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32)),
                       NULL);
  clux_value_t *shadow_cmp =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow_px, px, NULL);
  ASSERT_NE(shadow_cmp, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_cmp));
  EXPECT_EQ(clux_value_get_type(shadow_cmp),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));

  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_EQ, err, px, NULL), err);
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_NE, px, err, NULL), err);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, PtrAssignThroughViewRequiresBaseType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *p = clux_context_ref(ctx, x, NULL);
  clux_value_t *view = clux_context_deref(ctx, p, NULL);
  ASSERT_NE(view, nullptr);
  clux_value_t *wide = clux_context_create_i64(ctx, 2, NULL, NULL);
  clux_value_t *mismatch = clux_context_assign(ctx, view, wide, NULL);
  ASSERT_NE(mismatch, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(mismatch)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(mismatch),
               "cannot safely cast 'i64' to 'i32': narrowing conversion");
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

  /* deref 非指针类型：无 deref 槽位 */
  clux_value_t *deref_of_value = clux_context_deref(ctx, x, NULL);
  ASSERT_NE(deref_of_value, nullptr);
  EXPECT_STREQ(error_message_of(deref_of_value),
               "type 'i32' does not support operator '*'");
  /* index 语义是数组/切片取项：指针不提供下标 */
  clux_value_t *index_of_ptr = clux_context_index(
      ctx, p, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(index_of_ptr, nullptr);
  EXPECT_STREQ(error_message_of(index_of_ptr),
               "type '*i32' does not support indexing");

  clux_value_t *safe_cast = clux_context_safe_cast(
      ctx, p, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL);
  ASSERT_NE(safe_cast, nullptr);
  EXPECT_STREQ(error_message_of(safe_cast),
               "cannot safely cast '*i32' to 'i32'");

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

  clux_type_t *i32 = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i32_b = clux_context_builtin_type(other_ctx, CLUX_TYPE_I32);
  clux_type_t *i64 = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_type_t *str = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
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

  clux_type_t *i32 = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i32_b = clux_context_builtin_type(other_ctx, CLUX_TYPE_I32);
  clux_type_t *ptr_i32 = clux_type_get_ptr_type(i32_b);
  clux_type_t *ptr_i32_b = clux_type_get_ptr_type(i32);
  clux_type_t *ptr_i64 =
      clux_type_get_ptr_type(clux_context_builtin_type(ctx, CLUX_TYPE_I64));

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
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
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
  EXPECT_EQ(clux_value_get_type(sum),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
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
  clux_value_t *shadow_target = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *shadow_result =
      clux_context_assign(ctx, shadow_target, value, NULL);
  EXPECT_EQ(shadow_result, shadow_target);
  EXPECT_TRUE(clux_value_is_shadow(shadow_target));

  /* 右值 shadow：返回左值，数据不变 */
  clux_value_t *shadow_right = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
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
  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  clux_value_t *shadow_copy = clux_context_clone(ctx, shadow, NULL);
  ASSERT_NE(shadow_copy, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_copy));
  EXPECT_EQ(clux_value_get_type(shadow_copy),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, QualifiedTypeMetaAndInterning) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *const_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST);
  ASSERT_NE(const_i32, nullptr);
  EXPECT_EQ(clux_type_get_kind(const_i32), CLUX_TYPE_CONST);
  EXPECT_STREQ(clux_type_get_name(const_i32), "const i32");
  EXPECT_EQ(clux_type_get_size(const_i32), sizeof(int32_t));
  EXPECT_EQ(clux_type_get_align(const_i32), sizeof(int32_t));

  /* interning：同一 base 同一限定符恒等返回同一实例 */
  EXPECT_EQ(clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST), const_i32);

  /* volatile 独立成桶 */
  clux_type_t *volatile_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_VOLATILE);
  ASSERT_NE(volatile_i32, nullptr);
  EXPECT_EQ(clux_type_get_kind(volatile_i32), CLUX_TYPE_VOLATILE);
  EXPECT_STREQ(clux_type_get_name(volatile_i32), "volatile i32");
  EXPECT_NE(volatile_i32, const_i32);

  /* 嵌套：volatile const i32 */
  clux_type_t *volatile_const_i32 =
      clux_type_get_qualified_type(const_i32, CLUX_TYPE_VOLATILE);
  ASSERT_NE(volatile_const_i32, nullptr);
  EXPECT_STREQ(clux_type_get_name(volatile_const_i32), "volatile const i32");

  /* unwrap / is_const */
  EXPECT_EQ(clux_type_unwrap(const_i32), i32_type);
  EXPECT_EQ(clux_type_unwrap(volatile_const_i32), i32_type);
  EXPECT_TRUE(clux_type_is_const(const_i32));
  EXPECT_TRUE(clux_type_is_const(volatile_const_i32));
  EXPECT_FALSE(clux_type_is_const(volatile_i32));
  EXPECT_FALSE(clux_type_is_const(i32_type));
  EXPECT_EQ(clux_type_kind_unwrap(volatile_const_i32), CLUX_TYPE_I32);
  EXPECT_EQ(clux_type_kind_unwrap(i32_type), CLUX_TYPE_I32);

  /* 非法限定符入参返回 NULL */
  EXPECT_EQ(clux_type_get_qualified_type(i32_type, CLUX_TYPE_PTR), nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, QualifiedTypeEqualAndExtends) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *const_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST);
  clux_type_t *volatile_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_VOLATILE);

  /* 判等：限定不同即不同类型 */
  EXPECT_TRUE(clux_type_equal(const_i32, const_i32));
  EXPECT_TRUE(clux_type_equal(volatile_i32, volatile_i32));
  EXPECT_FALSE(clux_type_equal(const_i32, i32_type));
  EXPECT_FALSE(clux_type_equal(i32_type, const_i32));
  EXPECT_FALSE(clux_type_equal(const_i32, volatile_i32));

  /* extends：加限定是收紧，限定类型 extends 裸类型成立，反向不成立 */
  EXPECT_TRUE(clux_type_extends(const_i32, i32_type));
  EXPECT_TRUE(clux_type_extends(volatile_i32, i32_type));
  EXPECT_TRUE(clux_type_extends(const_i32, const_i32));
  EXPECT_FALSE(clux_type_extends(i32_type, const_i32));
  EXPECT_FALSE(clux_type_extends(i32_type, volatile_i32));
  EXPECT_FALSE(clux_type_extends(volatile_i32, const_i32));

  /* 限定类型上的指针：*const i32 与 *i32 是不同的指针类型 */
  clux_type_t *ptr_const = clux_context_get_ptr_type(ctx, const_i32);
  clux_type_t *ptr_plain = clux_context_get_ptr_type(ctx, i32_type);
  ASSERT_NE(ptr_const, nullptr);
  ASSERT_NE(ptr_plain, nullptr);
  EXPECT_FALSE(clux_type_equal(ptr_const, ptr_plain));
  EXPECT_TRUE(clux_type_equal(ptr_const, ptr_const));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, AssignRespectsConstQualifier) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *const_i32 = clux_type_get_qualified_type(
      clux_context_builtin_type(ctx, CLUX_TYPE_I32), CLUX_TYPE_CONST);
  clux_type_t *volatile_i32 = clux_type_get_qualified_type(
      clux_context_builtin_type(ctx, CLUX_TYPE_I32), CLUX_TYPE_VOLATILE);

  /* const 目标：直接建值并赋值被拒绝 */
  clux_value_t *x = clux_context_create_i32(ctx, 10, NULL, NULL);
  (void)x;
  clux_value_t *const_target = clux_context_create_shadow_value(ctx, const_i32);
  clux_value_t *value = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *bad = clux_context_assign(ctx, const_target, value, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "cannot assign to a value of type 'const i32'");

  /* volatile 目标：不阻断写入 */
  clux_value_t *volatile_target =
      clux_context_create_shadow_value(ctx, volatile_i32);
  clux_value_t *ok = clux_context_assign(ctx, volatile_target, value, NULL);
  EXPECT_EQ(ok, volatile_target);

  /* const 目标即便右值是 error 也先走 error 传播 */
  clux_value_t *err = clux_context_assign(ctx, NULL, NULL, NULL);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, const_target, err, NULL), err);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, DerefThroughConstPointer) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *const_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST);
  clux_type_t *ptr_const = clux_context_get_ptr_type(ctx, const_i32);

  clux_value_t *x = clux_context_create_i32(ctx, 10, NULL, NULL);
  clux_value_t *pcx = clux_context_create_ptr(
      ctx, ptr_const, clux_value_get_data(x), NULL, NULL);
  ASSERT_NE(pcx, nullptr);

  /* 解引用放行：返回带限定的 base 类型的 view */
  clux_value_t *read = clux_context_deref(ctx, pcx, NULL);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(clux_value_get_type(read), const_i32);
  EXPECT_EQ(read_i32(read), 10);

  /* 经 view 写入拒绝：const 是静态性质，shadow 右值也不例外 */
  clux_value_t *value = clux_context_create_i32(ctx, 99, NULL, NULL);
  clux_value_t *bad = clux_context_assign(ctx, read, value, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "cannot assign to a value of type 'const i32'");

  clux_value_t *shadow = clux_context_create_shadow_value(ctx, i32_type);
  clux_value_t *shadow_bad = clux_context_assign(ctx, read, shadow, NULL);
  ASSERT_NE(shadow_bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(shadow_bad)),
            CLUX_TYPE_ERROR);
  /* 原值未被写穿 */
  EXPECT_EQ(read_i32(x), 10);

  /* 带限定类型的值参与运算：unwrap 分派，产出裸类型结果 */
  clux_value_t *doubled = clux_context_add(ctx, read, read, NULL);
  ASSERT_NE(doubled, nullptr);
  EXPECT_EQ(clux_value_get_type(doubled), i32_type);
  EXPECT_EQ(read_i32(doubled), 20);

  clux_value_t *mixed = clux_context_add(ctx, x, read, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_EQ(clux_value_get_type(mixed), i32_type);
  EXPECT_EQ(read_i32(mixed), 20);

  /* const 值作为赋值右值合法 */
  clux_value_t *target = clux_context_create_i32(ctx, 0, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, target, read, NULL), target);
  EXPECT_EQ(read_i32(target), 10);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, DerefThroughVolatilePointer) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *volatile_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_VOLATILE);
  clux_type_t *ptr_volatile = clux_context_get_ptr_type(ctx, volatile_i32);

  clux_value_t *x = clux_context_create_i32(ctx, 10, NULL, NULL);
  clux_value_t *pvx = clux_context_create_ptr(
      ctx, ptr_volatile, clux_value_get_data(x), NULL, NULL);
  ASSERT_NE(pvx, nullptr);

  /* volatile 不阻断读 */
  clux_value_t *read = clux_context_deref(ctx, pvx, NULL);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(clux_value_get_type(read), volatile_i32);
  EXPECT_EQ(read_i32(read), 10);

  /* volatile 不阻断写：经 view 的通用 assign 放行并写穿别名 */
  clux_value_t *value = clux_context_create_i32(ctx, 42, NULL, NULL);
  clux_value_t *written = clux_context_assign(ctx, read, value, NULL);
  EXPECT_EQ(written, read);
  EXPECT_EQ(read_i32(x), 42);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- opaque：无类型载荷（FFI / 显式类型擦除边界，白皮书 §2.1）---- */

TEST(EngineTest, OpaqueLayoutAndNameLookup) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *opaque = clux_context_builtin_type(ctx, CLUX_TYPE_OPAQUE);
  ASSERT_NE(opaque, nullptr);
  EXPECT_STREQ(clux_type_get_name(opaque), "opaque");
  EXPECT_EQ(clux_type_get_size(opaque), sizeof(ptr_handle_t));
  EXPECT_EQ(clux_type_get_align(opaque), alignof(ptr_handle_t));

  /* 类型名与 identifier 合一：opaque 作为 type value 按名注册进 global */
  clux_value_t *found = clux_context_lookup(ctx, "opaque", NULL);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(clux_value_get_type(found), clux_context_get_type_type(ctx));
  void *raw = ptr_to_raw(clux_value_get_data(found), 0);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(*(clux_type_t **)raw, opaque);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OpaqueImplicitFromAnyPointer) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *opaque = clux_context_builtin_type(ctx, CLUX_TYPE_OPAQUE);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  clux_value_t *x = clux_context_create_i32(ctx, 10, NULL, NULL);
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  ASSERT_NE(p, nullptr);
  clux_value_t *slot = clux_context_create_ptr(ctx, opaque, 0, NULL, NULL);
  ASSERT_NE(slot, nullptr);

  /* 任何指针隐式转 opaque：assign 经 ptr 的 safe_cast 放行 */
  clux_value_t *assigned = clux_context_assign(ctx, slot, p, NULL);
  EXPECT_EQ(assigned, slot);
  EXPECT_EQ(clux_value_get_type(slot), opaque);

  /* 显式转回具体指针后 handle 未丢：可解引用读回原值 */
  clux_value_t *back = clux_context_cast(ctx, slot, ptr_i32, NULL);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(clux_value_get_type(back), ptr_i32);
  clux_value_t *read = clux_context_deref(ctx, back, NULL);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read_i32(read), 10);

  /* const 限定指针同样可隐式擦除 */
  clux_type_t *const_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST);
  clux_type_t *ptr_const = clux_context_get_ptr_type(ctx, const_i32);
  clux_value_t *pcx = clux_context_create_ptr(
      ctx, ptr_const, clux_value_get_data(x), NULL, NULL);
  clux_value_t *slot2 = clux_context_create_ptr(ctx, opaque, 0, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, slot2, pcx, NULL), slot2);

  /* opaque 也可显式转成限定指针（unwrap 分派） */
  clux_value_t *back_const = clux_context_cast(ctx, slot2, ptr_const, NULL);
  ASSERT_NE(back_const, nullptr);
  EXPECT_EQ(clux_value_get_type(back_const), ptr_const);
  clux_value_t *read_const = clux_context_deref(ctx, back_const, NULL);
  ASSERT_NE(read_const, nullptr);
  EXPECT_EQ(read_i32(read_const), 10);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OpaqueCastRejectsNonPointer) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *opaque = clux_context_builtin_type(ctx, CLUX_TYPE_OPAQUE);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  clux_value_t *x = clux_context_create_i32(ctx, 10, NULL, NULL);
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  clux_value_t *slot = clux_context_create_ptr(ctx, opaque, 0, NULL, NULL);
  ASSERT_EQ(clux_context_assign(ctx, slot, p, NULL), slot);

  /* 显式 as 只放行指针族：转基础类型拒绝 */
  clux_value_t *bad = clux_context_cast(ctx, slot, i32_type, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "cannot cast 'opaque' to 'i32'");

  /* 隐式转出拒绝：opaque → *i32 赋值必须显式 as */
  clux_value_t *ptr_slot = clux_context_create_ptr(ctx, ptr_i32, 0, NULL, NULL);
  clux_value_t *bad2 = clux_context_assign(ctx, ptr_slot, slot, NULL);
  ASSERT_NE(bad2, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad2)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad2), "cannot safely cast 'opaque' to '*i32'");

  /* 转自身是恒等 */
  EXPECT_EQ(clux_context_cast(ctx, slot, opaque, NULL), slot);

  /* 非 opaque 源经 i32 的 safe_cast 走到 opaque 也不放行：
     opaque 只从指针族隐式接收 */
  clux_value_t *n = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *bad3 = clux_context_assign(ctx, slot, n, NULL);
  ASSERT_NE(bad3, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad3)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad3), "cannot safely cast 'i32' to 'opaque'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OpaqueEqNe) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *opaque = clux_context_builtin_type(ctx, CLUX_TYPE_OPAQUE);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *y = clux_context_create_i32(ctx, 2, NULL, NULL);
  clux_value_t *ox = clux_context_create_value(
      ctx, opaque, clux_value_get_data(x), NULL, NULL);
  clux_value_t *ox2 = clux_context_create_value(
      ctx, opaque, clux_value_get_data(x), NULL, NULL);
  clux_value_t *oy = clux_context_create_value(
      ctx, opaque, clux_value_get_data(y), NULL, NULL);
  ASSERT_NE(ox, nullptr);
  ASSERT_NE(ox2, nullptr);
  ASSERT_NE(oy, nullptr);

  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, ox, ox2, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_EQ(read_bool(eq), true);
  clux_value_t *ne = clux_context_binary(ctx, CLUX_OP_NE, ox, ox2, NULL);
  ASSERT_NE(ne, nullptr);
  EXPECT_EQ(read_bool(ne), false);
  clux_value_t *eq2 = clux_context_binary(ctx, CLUX_OP_EQ, ox, oy, NULL);
  ASSERT_NE(eq2, nullptr);
  EXPECT_EQ(read_bool(eq2), false);
  clux_value_t *ne2 = clux_context_binary(ctx, CLUX_OP_NE, ox, oy, NULL);
  ASSERT_NE(ne2, nullptr);
  EXPECT_EQ(read_bool(ne2), true);

  /* 与指针比较拒绝：opaque 与 *T 不是同一类型 */
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  clux_value_t *bad = clux_context_binary(ctx, CLUX_OP_EQ, ox, p, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "operator '==' requires operands of the same type");

  /* shadow 比较产出 shadow bool */
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, opaque);
  clux_value_t *shadow_eq =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow, ox, NULL);
  ASSERT_NE(shadow_eq, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_eq));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OpaqueShadowPassthrough) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *opaque = clux_context_builtin_type(ctx, CLUX_TYPE_OPAQUE);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  /* shadow 指针隐式擦除：产出 shadow opaque，assign 静默返回目标 */
  clux_value_t *shadow_ptr = clux_context_create_shadow_value(ctx, ptr_i32);
  clux_value_t *slot = clux_context_create_ptr(ctx, opaque, 0, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, slot, shadow_ptr, NULL), slot);

  /* shadow opaque 显式转出：产出目标类型的 shadow */
  clux_value_t *shadow_opaque = clux_context_create_shadow_value(ctx, opaque);
  clux_value_t *out = clux_context_cast(ctx, shadow_opaque, ptr_i32, NULL);
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(out));
  EXPECT_EQ(clux_value_get_type(out), ptr_i32);

  /* shadow opaque 隐式转出依旧拒绝（shadow 不改变转换合法性） */
  clux_value_t *ptr_slot = clux_context_create_ptr(ctx, ptr_i32, 0, NULL, NULL);
  clux_value_t *bad = clux_context_assign(ctx, ptr_slot, shadow_opaque, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "cannot safely cast 'opaque' to '*i32'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SliceTypeInterningAndMeta) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *slice_i32 = clux_context_get_slice_type(ctx, i32_type);
  ASSERT_NE(slice_i32, nullptr);
  EXPECT_EQ(clux_type_get_kind(slice_i32), CLUX_TYPE_SLICE);
  EXPECT_STREQ(clux_type_get_name(slice_i32), "[]i32");
  EXPECT_EQ(clux_context_get_slice_type(ctx, i32_type), slice_i32);
  EXPECT_EQ(clux_type_get_size(slice_i32), sizeof(clux_slice_data_t));
  EXPECT_EQ(clux_type_get_align(slice_i32), alignof(clux_slice_data_t));

  /* 不同 base 产出不同实例；判等由 vtable 递归回答 */
  clux_type_t *slice_i64 = clux_context_get_slice_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  ASSERT_NE(slice_i64, nullptr);
  EXPECT_NE(slice_i64, slice_i32);
  EXPECT_TRUE(clux_type_equal(slice_i32, slice_i32));
  EXPECT_FALSE(clux_type_equal(slice_i32, slice_i64));
  EXPECT_TRUE(clux_type_extends(slice_i32, slice_i32));
  EXPECT_FALSE(clux_type_extends(slice_i32, slice_i64));
  /* 与指针类型互不相等 */
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);
  EXPECT_FALSE(clux_type_equal(slice_i32, ptr_i32));
  EXPECT_FALSE(clux_type_extends(slice_i32, ptr_i32));

  /* 元素类型带限定：[]const i32 与 []i32 区分 */
  clux_type_t *const_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST);
  clux_type_t *slice_const = clux_context_get_slice_type(ctx, const_i32);
  ASSERT_NE(slice_const, nullptr);
  EXPECT_STREQ(clux_type_get_name(slice_const), "[]const i32");
  EXPECT_FALSE(clux_type_equal(slice_const, slice_i32));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SliceCreateAndLen) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *slice_i32 = clux_context_get_slice_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *x = clux_context_create_i32(ctx, 42, NULL, NULL);
  clux_value_t *slice = clux_context_create_slice(
      ctx, slice_i32, clux_value_get_data(x), 4, "s", NULL);
  ASSERT_NE(slice, nullptr);
  EXPECT_EQ(clux_value_get_type(slice), slice_i32);

  /* data 布局：{ ptr, len }，ptr 借用 x 的内存 */
  clux_slice_data_t *raw =
      (clux_slice_data_t *)ptr_to_raw(clux_value_get_data(slice), 0);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(raw->ptr, clux_value_get_data(x));
  EXPECT_EQ(raw->len, 4u);

  /* len 读出为 usize */
  clux_value_t *len = clux_context_slice_len(ctx, slice, NULL);
  ASSERT_NE(len, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(len)), CLUX_TYPE_USIZE);
  EXPECT_EQ(read_usize(len), 4u);

  /* shadow 切片：len 产出 shadow usize */
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, slice_i32);
  clux_value_t *shadow_len = clux_context_slice_len(ctx, shadow, NULL);
  ASSERT_NE(shadow_len, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_len));
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(shadow_len)),
            CLUX_TYPE_USIZE);

  /* 非切片类型报 error */
  clux_value_t *bad = clux_context_slice_len(ctx, x, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "type 'i32' is not a slice");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, SliceCompareAndCloneAssign) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *slice_i32 = clux_context_get_slice_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *x = clux_context_create_i32(ctx, 42, NULL, NULL);
  ptr_handle_t target = clux_value_get_data(x);
  clux_value_t *s1 =
      clux_context_create_slice(ctx, slice_i32, target, 4, NULL, NULL);
  clux_value_t *s2 =
      clux_context_create_slice(ctx, slice_i32, target, 4, NULL, NULL);
  clux_value_t *s3 =
      clux_context_create_slice(ctx, slice_i32, target, 8, NULL, NULL);

  /* ptr 与 len 全等才相等 */
  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, s1, s2, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_TRUE(read_bool(eq));
  clux_value_t *ne = clux_context_binary(ctx, CLUX_OP_NE, s1, s3, NULL);
  ASSERT_NE(ne, nullptr);
  EXPECT_TRUE(read_bool(ne));

  /* 右操作数非切片报 error */
  clux_value_t *bad = clux_context_binary(ctx, CLUX_OP_EQ, s1, x, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "operator '==' requires operands of the same type");

  /* shadow 切片比较产出 shadow bool */
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, slice_i32);
  clux_value_t *shadow_eq =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow, s1, NULL);
  ASSERT_NE(shadow_eq, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_eq));

  /* clone：通用 memcpy 路径产出独立 data 块，字段一致 */
  clux_value_t *copy = clux_context_clone(ctx, s1, NULL);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(clux_value_get_type(copy), slice_i32);
  EXPECT_NE(clux_value_get_data(copy), clux_value_get_data(s1));
  clux_slice_data_t *copy_raw =
      (clux_slice_data_t *)ptr_to_raw(clux_value_get_data(copy), 0);
  EXPECT_EQ(copy_raw->ptr, target);
  EXPECT_EQ(copy_raw->len, 4u);

  /* assign：位图整体写穿，目标字段更新为右值字段 */
  clux_value_t *receiver =
      clux_context_create_slice(ctx, slice_i32, 0, 0, "r", NULL);
  EXPECT_EQ(clux_context_assign(ctx, receiver, s3, NULL), receiver);
  clux_slice_data_t *receiver_raw =
      (clux_slice_data_t *)ptr_to_raw(clux_value_get_data(receiver), 0);
  EXPECT_EQ(receiver_raw->ptr, target);
  EXPECT_EQ(receiver_raw->len, 8u);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ArrayTypeInterningAndMeta) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *arr3 = clux_context_get_array_type(ctx, i32_type, 3);
  ASSERT_NE(arr3, nullptr);
  EXPECT_EQ(clux_type_get_kind(arr3), CLUX_TYPE_ARRAY);
  EXPECT_STREQ(clux_type_get_name(arr3), "[3]i32");
  EXPECT_EQ(clux_type_get_size(arr3), 3 * sizeof(int32_t));
  EXPECT_EQ(clux_type_get_align(arr3), alignof(int32_t));
  EXPECT_EQ(clux_context_get_array_type(ctx, i32_type, 3), arr3);

  /* 不同长度/不同 base 均为不同实例 */
  clux_type_t *arr4 = clux_context_get_array_type(ctx, i32_type, 4);
  clux_type_t *arr3_i64 = clux_context_get_array_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I64), 3);
  EXPECT_NE(arr4, arr3);
  EXPECT_NE(arr3_i64, arr3);
  EXPECT_FALSE(clux_type_equal(arr3, arr4));
  EXPECT_FALSE(clux_type_equal(arr3, arr3_i64));
  EXPECT_TRUE(clux_type_equal(arr3, arr3));
  EXPECT_TRUE(clux_type_extends(arr3, arr3));
  EXPECT_FALSE(clux_type_extends(arr3, arr4));

  /* 元素带限定与裸元素区分 */
  clux_type_t *arr3_const = clux_context_get_array_type(
      ctx, clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST), 3);
  ASSERT_NE(arr3_const, nullptr);
  EXPECT_STREQ(clux_type_get_name(arr3_const), "[3]const i32");
  EXPECT_FALSE(clux_type_equal(arr3_const, arr3));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ArrayIndexReadWrite) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *arr_type = clux_context_get_array_type(ctx, i32_type, 3);
  clux_value_t *arr = clux_context_create_array(ctx, arr_type, "arr", NULL);
  ASSERT_NE(arr, nullptr);
  EXPECT_EQ(clux_value_get_type(arr), arr_type);

  /* 零初始化 */
  for (size_t i = 0; i < 3; i++) {
    clux_value_t *elem = clux_context_index(
        ctx, arr, clux_context_create_usize(ctx, i, NULL, NULL), NULL);
    ASSERT_NE(elem, nullptr);
    EXPECT_EQ(read_i32(elem), 0);
  }

  /* 写入并读回：读是 view，写入走通用 assign（返回 view 本身） */
  clux_value_t *v10 = clux_context_create_i32(ctx, 10, NULL, NULL);
  clux_value_t *v20 = clux_context_create_i32(ctx, 20, NULL, NULL);
  clux_value_t *view1 = clux_context_index(
      ctx, arr, clux_context_create_i32(ctx, 1, NULL, NULL), NULL);
  ASSERT_NE(view1, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, view1, v10, NULL), view1);
  clux_value_t *view2 = clux_context_index(
      ctx, arr, clux_context_create_u8(ctx, 2, NULL, NULL), NULL);
  ASSERT_NE(view2, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, view2, v20, NULL), view2);
  EXPECT_EQ(read_i32(clux_context_index(
                ctx, arr, clux_context_create_i64(ctx, 1, NULL, NULL), NULL)),
            10);
  EXPECT_EQ(read_i32(clux_context_index(
                ctx, arr, clux_context_create_usize(ctx, 2, NULL, NULL), NULL)),
            20);

  /* view 与数组内联块别名：写穿 view 后原数组可见变化 */
  EXPECT_EQ(read_i32(view1), 10);
  EXPECT_EQ(clux_context_assign(ctx, view1, v20, NULL), view1);
  EXPECT_EQ(read_i32(view1), 20);

  /* 越界与负下标 */
  clux_value_t *oob = clux_context_index(
      ctx, arr, clux_context_create_usize(ctx, 3, NULL, NULL), NULL);
  ASSERT_NE(oob, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(oob)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(oob),
               "index 3 is out of bounds for array of length 3");
  clux_value_t *neg = clux_context_index(
      ctx, arr, clux_context_create_i32(ctx, -1, NULL, NULL), NULL);
  ASSERT_NE(neg, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(neg)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(neg),
               "index -1 is out of bounds for array of length 3");

  /* 非整数下标 */
  clux_value_t *bad_idx = clux_context_index(
      ctx, arr, clux_context_create_bool(ctx, true, NULL, NULL), NULL);
  ASSERT_NE(bad_idx, nullptr);
  EXPECT_STREQ(error_message_of(bad_idx),
               "index must be an integer, got 'bool'");

  /* 类型不符的写入 */
  clux_value_t *bad_val = clux_context_assign(
      ctx,
      clux_context_index(
          ctx, arr, clux_context_create_usize(ctx, 0, NULL, NULL), NULL),
      clux_context_create_str(ctx, "x", NULL, NULL),
      NULL);
  ASSERT_NE(bad_val, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_val)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_val),
               "type 'str' cannot be safely cast to 'i32'");

  /* shadow：下标读产出 base 类型的 shadow，经 assign 写入仍为 shadow */
  clux_value_t *arr_shadow = clux_context_create_shadow_value(ctx, arr_type);
  clux_value_t *shadow_get = clux_context_index(
      ctx, arr_shadow, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(shadow_get, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_get));
  EXPECT_EQ(clux_value_get_type(shadow_get), i32_type);
  clux_value_t *shadow_set = clux_context_assign(ctx, shadow_get, v10, NULL);
  ASSERT_NE(shadow_set, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_set));

  /* 不支持索引的类型 */
  clux_value_t *not_indexable = clux_context_index(
      ctx, v10, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(not_indexable, nullptr);
  EXPECT_STREQ(error_message_of(not_indexable),
               "type 'i32' does not support indexing");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, ArrayConstElementAndCompareAndClone) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *const_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST);
  clux_type_t *const_arr = clux_context_get_array_type(ctx, const_i32, 2);

  /* const 元素数组：读放行，经 view 的写入静态拒绝（shadow 右值也不例外） */
  clux_value_t *carr = clux_context_create_array(ctx, const_arr, NULL, NULL);
  clux_value_t *read = clux_context_index(
      ctx, carr, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(clux_value_get_type(read), const_i32);
  clux_value_t *v = clux_context_create_i32(ctx, 9, NULL, NULL);
  clux_value_t *bad = clux_context_assign(ctx, read, v, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "cannot assign to a value of type 'const i32'");

  /* 裸元素接受带限定来源（safe_cast 经 unwrap 解包） */
  clux_type_t *arr_type = clux_context_get_array_type(ctx, i32_type, 2);
  clux_value_t *arr = clux_context_create_array(ctx, arr_type, NULL, NULL);
  clux_value_t *slot0 = clux_context_index(
      ctx, arr, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(slot0, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, slot0, read, NULL), slot0);

  /* eq/ne：整块内容比较 */
  clux_value_t *same1 = clux_context_create_array(ctx, arr_type, NULL, NULL);
  clux_value_t *same2 = clux_context_create_array(ctx, arr_type, NULL, NULL);
  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, same1, same2, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_TRUE(read_bool(eq));
  clux_value_t *v7 = clux_context_create_i32(ctx, 7, NULL, NULL);
  (void)clux_context_assign(
      ctx,
      clux_context_index(
          ctx, same2, clux_context_create_usize(ctx, 1, NULL, NULL), NULL),
      v7,
      NULL);
  clux_value_t *ne = clux_context_binary(ctx, CLUX_OP_NE, same1, same2, NULL);
  ASSERT_NE(ne, nullptr);
  EXPECT_TRUE(read_bool(ne));

  /* clone：独立内联块，内容一致 */
  clux_value_t *copy = clux_context_clone(ctx, same2, NULL);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(clux_value_get_type(copy), arr_type);
  EXPECT_NE(clux_value_get_data(copy), clux_value_get_data(same2));
  EXPECT_EQ(
      read_i32(clux_context_index(
          ctx, copy, clux_context_create_usize(ctx, 1, NULL, NULL), NULL)),
      7);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OptionalTypeInterningAndMeta) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *opt_i32 = clux_context_get_optional_type(ctx, i32_type);
  ASSERT_NE(opt_i32, nullptr);
  EXPECT_EQ(clux_type_get_kind(opt_i32), CLUX_TYPE_OPTIONAL);
  EXPECT_STREQ(clux_type_get_name(opt_i32), "?i32");
  /* 布局：首字节 present 标志 + 按 base 对齐的 payload */
  EXPECT_EQ(clux_type_get_size(opt_i32), 4 + sizeof(int32_t));
  EXPECT_EQ(clux_type_get_align(opt_i32), alignof(int32_t));
  EXPECT_EQ(clux_context_get_optional_type(ctx, i32_type), opt_i32);
  EXPECT_EQ(clux_type_get_optional_base(opt_i32), i32_type);

  /* 不同 base / 带限定载荷均为不同实例 */
  clux_type_t *opt_i64 = clux_context_get_optional_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  clux_type_t *opt_const = clux_context_get_optional_type(
      ctx, clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST));
  ASSERT_NE(opt_i64, nullptr);
  ASSERT_NE(opt_const, nullptr);
  EXPECT_STREQ(clux_type_get_name(opt_const), "?const i32");
  EXPECT_NE(opt_i64, opt_i32);
  EXPECT_NE(opt_const, opt_i32);
  EXPECT_TRUE(clux_type_equal(opt_i32, opt_i32));
  EXPECT_FALSE(clux_type_equal(opt_i32, opt_i64));
  EXPECT_TRUE(clux_type_extends(opt_i32, opt_i32));
  EXPECT_FALSE(clux_type_extends(opt_i32, opt_i64));

  /* optional 嵌套：??T 的载荷是 ?T */
  clux_type_t *opt_opt = clux_context_get_optional_type(ctx, opt_i32);
  ASSERT_NE(opt_opt, nullptr);
  EXPECT_STREQ(clux_type_get_name(opt_opt), "??i32");
  EXPECT_EQ(clux_type_get_optional_base(opt_opt), opt_i32);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OptionalSomeNoneAndUnwrap) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *opt_i32 = clux_context_get_optional_type(ctx, i32_type);
  clux_value_t *v5 = clux_context_create_i32(ctx, 5, NULL, NULL);
  clux_value_t *some =
      clux_context_create_optional_some(ctx, i32_type, v5, "some", NULL);
  ASSERT_NE(some, nullptr);
  EXPECT_EQ(clux_value_get_type(some), opt_i32);
  EXPECT_FALSE(clux_value_is_shadow(some));

  /* #take（.!）返回 base 类型的 view */
  clux_value_t *payload = clux_context_take(ctx, some, NULL);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(clux_value_get_type(payload), i32_type);
  EXPECT_EQ(read_i32(payload), 5);

  /* take 是借引用：写穿 view 更新 optional 载荷 */
  clux_value_t *v9 = clux_context_create_i32(ctx, 9, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, payload, v9, NULL), payload);
  EXPECT_EQ(read_i32(clux_context_take(ctx, some, NULL)), 9);

  /* none 的 take 是运行期错误 */
  clux_value_t *none =
      clux_context_create_optional_none(ctx, i32_type, NULL, NULL);
  ASSERT_NE(none, nullptr);
  clux_value_t *bad = clux_context_take(ctx, none, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "cannot take a value from a none optional of type '?i32'");

  /* deref 槽位回归指针专属：optional 走 *p 解引用报不支持 */
  clux_value_t *deref_opt = clux_context_deref(ctx, some, NULL);
  ASSERT_NE(deref_opt, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(deref_opt)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(deref_opt),
               "type '?i32' does not support operator '*'");

  /* shadow：take 产出载荷类型的 shadow */
  clux_value_t *shadow_opt = clux_context_create_shadow_value(ctx, opt_i32);
  clux_value_t *shadow_take = clux_context_take(ctx, shadow_opt, NULL);
  ASSERT_NE(shadow_take, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_take));
  EXPECT_EQ(clux_value_get_type(shadow_take), i32_type);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OptionalIsNoneQuery) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *opt_i32 = clux_context_get_optional_type(ctx, i32_type);
  clux_value_t *v5 = clux_context_create_i32(ctx, 5, NULL, NULL);
  clux_value_t *some =
      clux_context_create_optional_some(ctx, i32_type, v5, NULL, NULL);
  clux_value_t *none =
      clux_context_create_optional_none(ctx, i32_type, NULL, NULL);

  /* #is_none：none 为真 */
  EXPECT_FALSE(read_bool(clux_context_is_none(ctx, some, NULL)));
  EXPECT_TRUE(read_bool(clux_context_is_none(ctx, none, NULL)));

  /* 非 optional 类型不支持 */
  clux_value_t *bad = clux_context_is_none(ctx, v5, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "#is_none requires an optional value, got 'i32'");

  /* shadow 产出 bool shadow，error 自传播 */
  clux_value_t *shadow_opt = clux_context_create_shadow_value(ctx, opt_i32);
  clux_value_t *shadow_result = clux_context_is_none(ctx, shadow_opt, NULL);
  ASSERT_NE(shadow_result, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_result));
  EXPECT_EQ(clux_value_get_type(shadow_result),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_is_none(ctx, err, NULL), err);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OptionalCompareSameType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *opt_i32 = clux_context_get_optional_type(ctx, i32_type);
  clux_value_t *v5 = clux_context_create_i32(ctx, 5, NULL, NULL);
  clux_value_t *some5 =
      clux_context_create_optional_some(ctx, i32_type, v5, NULL, NULL);
  clux_value_t *some5b =
      clux_context_create_optional_some(ctx, i32_type, v5, NULL, NULL);
  clux_value_t *some7 = clux_context_create_optional_some(
      ctx, i32_type, clux_context_create_i32(ctx, 7, NULL, NULL), NULL, NULL);
  clux_value_t *none =
      clux_context_create_optional_none(ctx, i32_type, NULL, NULL);

  /* 同类型比较：present + payload */
  EXPECT_TRUE(
      read_bool(clux_context_binary(ctx, CLUX_OP_EQ, some5, some5b, NULL)));
  EXPECT_FALSE(
      read_bool(clux_context_binary(ctx, CLUX_OP_EQ, some5, some7, NULL)));
  EXPECT_TRUE(
      read_bool(clux_context_binary(ctx, CLUX_OP_NE, some5, some7, NULL)));
  EXPECT_FALSE(
      read_bool(clux_context_binary(ctx, CLUX_OP_EQ, some5, none, NULL)));
  EXPECT_TRUE(
      read_bool(clux_context_binary(ctx, CLUX_OP_EQ, none, none, NULL)));

  /* 类型不匹配 */
  clux_type_t *opt_i64 = clux_context_get_optional_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_STREQ(clux_type_get_name(opt_i64), "?i64");
  clux_value_t *some64 = clux_context_create_optional_some(
      ctx,
      clux_context_builtin_type(ctx, CLUX_TYPE_I64),
      clux_context_create_i64(ctx, 1, NULL, NULL),
      NULL,
      NULL);
  clux_value_t *mixed_value =
      clux_context_binary(ctx, CLUX_OP_EQ, some5, v5, NULL);
  ASSERT_NE(mixed_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(mixed_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(mixed_value),
               "operator '==' requires operands of the same type");
  clux_value_t *mixed_optional =
      clux_context_binary(ctx, CLUX_OP_EQ, some5, some64, NULL);
  ASSERT_NE(mixed_optional, nullptr);
  EXPECT_STREQ(error_message_of(mixed_optional),
               "operator '==' requires operands of the same type");

  /* shadow 与 error 传播 */
  clux_value_t *shadow_opt = clux_context_create_shadow_value(ctx, opt_i32);
  clux_value_t *shadow_cmp =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow_opt, some5, NULL);
  ASSERT_NE(shadow_cmp, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_cmp));
  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "boom");
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_EQ, err, some5, NULL), err);
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_EQ, some5, err, NULL), err);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, OptionalConstructionAndAssign) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *opt_i32 = clux_context_get_optional_type(ctx, i32_type);

  /* #none(i32)：载荷类型入参，产出 ?T{none}（零值） */
  clux_value_t *opt =
      clux_context_create_optional_none(ctx, i32_type, "o", NULL);
  ASSERT_NE(opt, nullptr);
  EXPECT_EQ(clux_value_get_type(opt), opt_i32);
  EXPECT_TRUE(read_bool(clux_context_is_none(ctx, opt, NULL)));

  /* #some(i32, v)：产出 ?T{v} */
  clux_value_t *v7 = clux_context_create_i32(ctx, 7, NULL, NULL);
  clux_value_t *some =
      clux_context_create_optional_some(ctx, i32_type, v7, NULL, NULL);
  ASSERT_NE(some, nullptr);
  EXPECT_EQ(clux_value_get_type(some), opt_i32);
  EXPECT_FALSE(read_bool(clux_context_is_none(ctx, some, NULL)));
  EXPECT_EQ(read_i32(clux_context_take(ctx, some, NULL)), 7);

  /* #some 的 value 类型自检 */
  clux_value_t *bad_some = clux_context_create_optional_some(
      ctx, i32_type, clux_context_create_i64(ctx, 1, NULL, NULL), NULL, NULL);
  ASSERT_NE(bad_some, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_some)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_some),
               "#some requires a value of type 'i32', got 'i64'");

  /* 同类型 optional 赋值：整体拷贝（含 present 标志） */
  EXPECT_EQ(clux_context_assign(ctx, opt, some, NULL), opt);
  EXPECT_FALSE(read_bool(clux_context_is_none(ctx, opt, NULL)));
  EXPECT_EQ(read_i32(clux_context_take(ctx, opt, NULL)), 7);

  /* base 值不经 assign 隐式包装：走 safe_cast 错误 */
  clux_value_t *no_wrap = clux_context_assign(ctx, opt, v7, NULL);
  ASSERT_NE(no_wrap, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(no_wrap)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(no_wrap), "cannot safely cast 'i32' to '?i32'");
  clux_value_t *bad_type = clux_context_assign(
      ctx, opt, clux_context_create_str(ctx, "x", NULL, NULL), NULL);
  ASSERT_NE(bad_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_type)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_type),
               "type 'str' cannot be safely cast to '?i32'");

  /* shadow 通道：不产出现值 */
  clux_value_t *shadow_opt = clux_context_create_shadow_value(ctx, opt_i32);
  EXPECT_EQ(clux_context_assign(ctx, shadow_opt, some, NULL), shadow_opt);
  EXPECT_TRUE(clux_value_is_shadow(shadow_opt));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, CunionTypeAndFieldAccess) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);

  /* 创建：size/align 取成员最大值（C ABI 布局） */
  clux_cunion_member_t members[] = {
      {"i", i32_type},
      {"l", i64_type},
  };
  clux_type_t *value_type =
      clux_context_create_cunion_type(ctx, "Value", members, 2);
  ASSERT_NE(value_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(value_type), CLUX_TYPE_CUNION);
  EXPECT_STREQ(clux_type_get_name(value_type), "Value");
  EXPECT_EQ(clux_type_get_size(value_type), 8u);
  EXPECT_EQ(clux_type_get_align(value_type), 8u);
  EXPECT_EQ(clux_cunion_member_count(value_type), 2u);
  EXPECT_EQ(clux_cunion_get_member(value_type, 0)->type, i32_type);
  EXPECT_EQ(clux_cunion_get_member(value_type, 2), nullptr);
  EXPECT_NE(clux_cunion_find_member(value_type, "l"), nullptr);
  EXPECT_EQ(clux_cunion_find_member(value_type, "x"), nullptr);

  /* 结构去重：成员名+类型按声明序一致即恒等返回同一实例 */
  clux_cunion_member_t same[] = {
      {"i", i32_type},
      {"l", i64_type},
  };
  EXPECT_EQ(clux_context_create_cunion_type(ctx, "Other", same, 2), value_type);
  clux_cunion_member_t swapped[] = {
      {"l", i64_type},
      {"i", i32_type},
  };
  clux_type_t *swapped_type =
      clux_context_create_cunion_type(ctx, "Swapped", swapped, 2);
  ASSERT_NE(swapped_type, nullptr);
  EXPECT_NE(swapped_type, value_type);
  EXPECT_FALSE(clux_type_equal(value_type, swapped_type));

  /* 非法入参：缺名、空成员表、重名、成员缺类型 */
  EXPECT_EQ(clux_context_create_cunion_type(ctx, NULL, members, 2), nullptr);
  EXPECT_EQ(clux_context_create_cunion_type(ctx, "E", members, 0), nullptr);
  EXPECT_EQ(clux_context_create_cunion_type(ctx, "E", NULL, 2), nullptr);
  clux_cunion_member_t dup[] = {
      {"i", i32_type},
      {"i", i64_type},
  };
  EXPECT_EQ(clux_context_create_cunion_type(ctx, "Dup", dup, 2), nullptr);
  clux_cunion_member_t bad[] = {
      {"i", i32_type},
      {"x", NULL},
  };
  EXPECT_EQ(clux_context_create_cunion_type(ctx, "Bad", bad, 2), nullptr);

  /* 零值创建：memset 清零 */
  clux_value_t *v = clux_context_create_cunion(ctx, value_type, "v", NULL);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(clux_value_get_type(v), value_type);
  EXPECT_EQ(read_i32(clux_context_field(ctx, v, "i", NULL)), 0);

  /* field 返回偏移 0 的成员 view：写穿 view 即写 union 内存 */
  clux_value_t *i_field = clux_context_field(ctx, v, "i", NULL);
  ASSERT_NE(i_field, nullptr);
  EXPECT_EQ(clux_value_get_type(i_field), i32_type);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, i_field, five, NULL), i_field);
  EXPECT_EQ(read_i32(clux_context_field(ctx, v, "i", NULL)), 5);

  /* 成员重叠：写 l 后 i 读到低 4 字节 */
  clux_value_t *l_field = clux_context_field(ctx, v, "l", NULL);
  ASSERT_NE(l_field, nullptr);
  EXPECT_EQ(clux_value_get_type(l_field), i64_type);
  clux_value_t *big = clux_context_create_i64(ctx, 0x100000005LL, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, l_field, big, NULL), l_field);
  EXPECT_EQ(read_i32(i_field), 5);

  /* 未知成员 */
  clux_value_t *missing = clux_context_field(ctx, v, "x", NULL);
  ASSERT_NE(missing, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(missing)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(missing), "cunion 'Value' has no member 'x'");

  /* 非 cunion 类型不支持 field 访问 */
  clux_value_t *plain = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *no_field = clux_context_field(ctx, plain, "i", NULL);
  ASSERT_NE(no_field, nullptr);
  EXPECT_STREQ(error_message_of(no_field),
               "type 'i32' does not support field access");

  /* create_cunion 拒绝非 cunion 类型 */
  clux_value_t *bad_value =
      clux_context_create_cunion(ctx, i32_type, NULL, NULL);
  ASSERT_NE(bad_value, nullptr);
  EXPECT_STREQ(error_message_of(bad_value),
               "cunion value creation requires a cunion type");

  /* shadow：field 产出成员类型的 shadow */
  clux_value_t *shadow_union =
      clux_context_create_shadow_value(ctx, value_type);
  clux_value_t *shadow_field = clux_context_field(ctx, shadow_union, "i", NULL);
  ASSERT_NE(shadow_field, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_field));
  EXPECT_EQ(clux_value_get_type(shadow_field), i32_type);

  /* eq/ne 按字节：v 含 0x1_00000005，与零值 other 不等；写同值后相等，
     再改一字节不等 */
  clux_value_t *other = clux_context_create_cunion(ctx, value_type, NULL, NULL);
  ASSERT_NE(other, nullptr);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, v, other, NULL)));
  clux_value_t *other_l = clux_context_field(ctx, other, "l", NULL);
  EXPECT_EQ(clux_context_assign(ctx, other_l, big, NULL), other_l);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_EQ, v, other, NULL)));
  clux_value_t *other_i = clux_context_field(ctx, other, "i", NULL);
  EXPECT_EQ(
      clux_context_assign(
          ctx, other_i, clux_context_create_i32(ctx, 6, NULL, NULL), NULL),
      other_i);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, v, other, NULL)));
  clux_value_t *mixed = clux_context_binary(ctx, CLUX_OP_EQ, v, plain, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_STREQ(error_message_of(mixed),
               "operator '==' requires operands of the same type");

  /* 同类型 assign 整体拷贝 + clone */
  EXPECT_EQ(read_i32(clux_context_field(
                ctx, clux_context_assign(ctx, other, v, NULL), "i", NULL)),
            5);
  clux_value_t *copy = clux_context_clone(ctx, v, NULL);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(read_i32(clux_context_field(ctx, copy, "i", NULL)), 5);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_EQ, copy, v, NULL)));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, TagUnionTypeAndIsMember) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);

  /* 创建：tag u32 在偏移 0，payload 从对齐处开始
     （align = max(4, 成员对齐) = 8，size = round_up(8 + 8, 8) = 16） */
  clux_union_member_t members[] = {
      {"a", i32_type},
      {"b", i64_type},
  };
  clux_type_t *value_type =
      clux_context_create_union_type(ctx, "Value", members, 2);
  ASSERT_NE(value_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(value_type), CLUX_TYPE_UNION);
  EXPECT_STREQ(clux_type_get_name(value_type), "Value");
  EXPECT_EQ(clux_type_get_size(value_type), 16u);
  EXPECT_EQ(clux_type_get_align(value_type), 8u);
  EXPECT_EQ(clux_union_payload_offset(value_type), 8u);
  EXPECT_EQ(clux_union_member_count(value_type), 2u);
  EXPECT_EQ(clux_union_get_member(value_type, 1)->type, i64_type);
  EXPECT_EQ(clux_union_get_member(value_type, 2), nullptr);
  EXPECT_NE(clux_union_find_member(value_type, "b"), nullptr);
  EXPECT_EQ(clux_union_find_member(value_type, "x"), nullptr);

  /* 结构去重：成员名+类型按声明序一致即恒等返回同一实例 */
  clux_union_member_t same[] = {
      {"a", i32_type},
      {"b", i64_type},
  };
  EXPECT_EQ(clux_context_create_union_type(ctx, "Other", same, 2), value_type);
  clux_union_member_t swapped[] = {
      {"b", i64_type},
      {"a", i32_type},
  };
  clux_type_t *swapped_type =
      clux_context_create_union_type(ctx, "Swapped", swapped, 2);
  ASSERT_NE(swapped_type, nullptr);
  EXPECT_NE(swapped_type, value_type);
  EXPECT_FALSE(clux_type_equal(value_type, swapped_type));

  /* 非法入参：缺名、空成员表、成员缺类型、重名 */
  EXPECT_EQ(clux_context_create_union_type(ctx, NULL, members, 2), nullptr);
  EXPECT_EQ(clux_context_create_union_type(ctx, "E", members, 0), nullptr);
  EXPECT_EQ(clux_context_create_union_type(ctx, "E", NULL, 2), nullptr);
  clux_union_member_t bad[] = {
      {"a", i32_type},
      {"x", NULL},
  };
  EXPECT_EQ(clux_context_create_union_type(ctx, "Bad", bad, 2), nullptr);
  clux_union_member_t dup[] = {
      {"a", i32_type},
      {"a", i64_type},
  };
  EXPECT_EQ(clux_context_create_union_type(ctx, "Dup", dup, 2), nullptr);

  /* 零值创建：tag=0 即第一个成员 a 激活 */
  clux_value_t *v = clux_context_create_union(ctx, value_type, "v", NULL);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(clux_value_get_type(v), value_type);
  EXPECT_TRUE(read_bool(clux_context_is_member(ctx, v, "a", NULL)));
  EXPECT_FALSE(read_bool(clux_context_is_member(ctx, v, "b", NULL)));

  /* field：tag 匹配时返回 payload 的成员 view，写穿 view */
  clux_value_t *a_field = clux_context_field(ctx, v, "a", NULL);
  ASSERT_NE(a_field, nullptr);
  EXPECT_EQ(clux_value_get_type(a_field), i32_type);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, a_field, five, NULL), a_field);
  EXPECT_EQ(read_i32(clux_context_field(ctx, v, "a", NULL)), 5);

  /* tag 不匹配报运行期 error（先 is 判定再读取） */
  clux_value_t *wrong = clux_context_field(ctx, v, "b", NULL);
  ASSERT_NE(wrong, nullptr);
  EXPECT_STREQ(error_message_of(wrong),
               "union is not 'b': current member is 'a', test with 'is' first");

  /* set_member 激活：切 tag 并清零 payload */
  EXPECT_EQ(clux_context_union_set_member(ctx, v, "b", NULL), v);
  EXPECT_FALSE(read_bool(clux_context_is_member(ctx, v, "a", NULL)));
  EXPECT_TRUE(read_bool(clux_context_is_member(ctx, v, "b", NULL)));
  clux_value_t *b_field = clux_context_field(ctx, v, "b", NULL);
  ASSERT_NE(b_field, nullptr);
  EXPECT_EQ(read_i64(b_field), 0);
  clux_value_t *seven = clux_context_create_i64(ctx, 7, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, b_field, seven, NULL), b_field);
  EXPECT_EQ(read_i64(clux_context_field(ctx, v, "b", NULL)), 7);
  clux_value_t *now_wrong = clux_context_field(ctx, v, "a", NULL);
  ASSERT_NE(now_wrong, nullptr);
  EXPECT_STREQ(error_message_of(now_wrong),
               "union is not 'a': current member is 'b', test with 'is' first");

  /* 未知成员：field / is_member / set_member 三路报同样的名 */
  clux_value_t *missing = clux_context_field(ctx, v, "x", NULL);
  ASSERT_NE(missing, nullptr);
  EXPECT_STREQ(error_message_of(missing), "union 'Value' has no member 'x'");
  clux_value_t *missing_is = clux_context_is_member(ctx, v, "x", NULL);
  ASSERT_NE(missing_is, nullptr);
  EXPECT_STREQ(error_message_of(missing_is), "union 'Value' has no member 'x'");
  clux_value_t *missing_set = clux_context_union_set_member(ctx, v, "x", NULL);
  ASSERT_NE(missing_set, nullptr);
  EXPECT_STREQ(error_message_of(missing_set),
               "union 'Value' has no member 'x'");

  /* 非 union 类型的兜底报错 */
  clux_value_t *plain = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *bad_create =
      clux_context_create_union(ctx, i32_type, NULL, NULL);
  ASSERT_NE(bad_create, nullptr);
  EXPECT_STREQ(error_message_of(bad_create),
               "union value creation requires a union type");
  clux_value_t *bad_is = clux_context_is_member(ctx, plain, "a", NULL);
  ASSERT_NE(bad_is, nullptr);
  EXPECT_STREQ(error_message_of(bad_is),
               "operator 'is' requires a tag union value, got 'i32'");
  clux_value_t *bad_set = clux_context_union_set_member(ctx, plain, "a", NULL);
  ASSERT_NE(bad_set, nullptr);
  EXPECT_STREQ(error_message_of(bad_set),
               "member activation requires a union value, got 'i32'");

  /* shadow：field 产出成员 shadow，is_member 产出 bool shadow，
     set_member 原样返回 */
  clux_value_t *shadow_union =
      clux_context_create_shadow_value(ctx, value_type);
  clux_value_t *shadow_field = clux_context_field(ctx, shadow_union, "a", NULL);
  ASSERT_NE(shadow_field, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_field));
  EXPECT_EQ(clux_value_get_type(shadow_field), i32_type);
  clux_value_t *shadow_is =
      clux_context_is_member(ctx, shadow_union, "a", NULL);
  ASSERT_NE(shadow_is, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_is));
  EXPECT_EQ(clux_value_get_type(shadow_is),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  EXPECT_EQ(clux_context_union_set_member(ctx, shadow_union, "b", NULL),
            shadow_union);

  /* eq/ne 递归（白皮书 §5.5）：先比 tag，相同再按该成员载荷递归比较 */
  clux_value_t *other = clux_context_create_union(ctx, value_type, NULL, NULL);
  ASSERT_NE(other, nullptr);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, v, other, NULL)));
  EXPECT_EQ(clux_context_union_set_member(ctx, other, "b", NULL), other);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, v, other, NULL)));
  clux_value_t *other_b = clux_context_field(ctx, other, "b", NULL);
  EXPECT_EQ(clux_context_assign(ctx, other_b, seven, NULL), other_b);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_EQ, v, other, NULL)));
  /* tag 不同即不等，即使 payload 位相同 */
  EXPECT_EQ(clux_context_union_set_member(ctx, other, "a", NULL), other);
  clux_value_t *other_a = clux_context_field(ctx, other, "a", NULL);
  EXPECT_EQ(clux_context_assign(ctx, other_a, five, NULL), other_a);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, v, other, NULL)));
  clux_value_t *mixed = clux_context_binary(ctx, CLUX_OP_EQ, v, plain, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_STREQ(error_message_of(mixed),
               "operator '==' requires operands of the same type");
  clux_value_t *cross = clux_context_binary(ctx, CLUX_OP_EQ, v, other, NULL);
  ASSERT_NE(cross, nullptr);

  /* 同类型 assign 整体拷贝（tag+payload）+ clone */
  EXPECT_EQ(clux_context_union_set_member(ctx, other, "b", NULL), other);
  EXPECT_EQ(read_i64(clux_context_field(
                ctx, clux_context_assign(ctx, other, v, NULL), "b", NULL)),
            7);
  clux_value_t *copy = clux_context_clone(ctx, v, NULL);
  ASSERT_NE(copy, nullptr);
  EXPECT_TRUE(read_bool(clux_context_is_member(ctx, copy, "b", NULL)));
  EXPECT_EQ(read_i64(clux_context_field(ctx, copy, "b", NULL)), 7);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_EQ, copy, v, NULL)));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, FuncTypeInvokeAndCaptures) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);

  /* 创建：头部 AST 指针在偏移 0，捕获字段从对齐处平铺
     （align = max(8, 4, 8) = 8；a 偏移 8，b 偏移 round_up(12, 8) = 16，
     末尾 16 + 8 = 24 已对齐，size = 24） */
  clux_func_capture_t captures[] = {
      {"a", i32_type},
      {"b", i64_type},
  };
  clux_type_t *params[] = {i32_type, i32_type};
  clux_type_t *func_type = clux_context_create_func_type(
      ctx, "func|a:i32,b:i64|(i32,i32)->i32", captures, 2, params, 2, i32_type);
  ASSERT_NE(func_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(func_type), CLUX_TYPE_FUNC);
  EXPECT_STREQ(clux_type_get_name(func_type),
               "func|a:i32,b:i64|(i32,i32)->i32");
  EXPECT_EQ(clux_type_get_size(func_type), 24u);
  EXPECT_EQ(clux_type_get_align(func_type), 8u);
  EXPECT_EQ(clux_func_ast_offset(), 0u);
  EXPECT_EQ(clux_func_capture_count(func_type), 2u);
  EXPECT_EQ(clux_func_get_capture(func_type, 1)->type, i64_type);
  EXPECT_EQ(clux_func_get_capture(func_type, 2), nullptr);
  EXPECT_NE(clux_func_find_capture(func_type, "b"), nullptr);
  EXPECT_EQ(clux_func_find_capture(func_type, "x"), nullptr);
  EXPECT_EQ(clux_func_capture_offset(func_type, 0), 8u);
  EXPECT_EQ(clux_func_capture_offset(func_type, 1), 16u);
  EXPECT_EQ(clux_func_param_count(func_type), 2u);
  EXPECT_EQ(clux_func_get_param(func_type, 1), i32_type);
  EXPECT_EQ(clux_func_get_param(func_type, 2), nullptr);
  EXPECT_EQ(clux_func_get_return_type(func_type), i32_type);

  /* 结构去重：捕获表+参数序列+返回类型按声明序一致即恒等返回 */
  EXPECT_EQ(clux_context_create_func_type(
                ctx, "Other", captures, 2, params, 2, i32_type),
            func_type);
  /* 无捕获（func(i32,i32)->i32）与带捕获是不同类型（无闭包擦除） */
  clux_type_t *plain_type = clux_context_create_func_type(
      ctx, "func(i32,i32)->i32", NULL, 0, params, 2, i32_type);
  ASSERT_NE(plain_type, nullptr);
  EXPECT_NE(plain_type, func_type);
  EXPECT_FALSE(clux_type_equal(func_type, plain_type));
  EXPECT_EQ(clux_type_get_size(plain_type), 8u); /* 头部指针即全部 */
  /* 捕获类型不同 / 捕获名不同 / 捕获序不同 / 参数不同 / 返回不同 */
  clux_func_capture_t cap_i64[] = {{"a", i64_type}};
  EXPECT_NE(
      clux_context_create_func_type(ctx, "C1", cap_i64, 1, params, 2, i32_type),
      func_type);
  clux_func_capture_t cap_renamed[] = {{"c", i32_type}};
  EXPECT_NE(clux_context_create_func_type(
                ctx, "C2", cap_renamed, 1, params, 2, i32_type),
            func_type);
  clux_func_capture_t cap_swapped[] = {
      {"b", i64_type},
      {"a", i32_type},
  };
  EXPECT_NE(clux_context_create_func_type(
                ctx, "C3", cap_swapped, 2, params, 2, i32_type),
            func_type);
  clux_type_t *swapped_params[] = {i64_type, i32_type};
  EXPECT_NE(clux_context_create_func_type(
                ctx, "C4", captures, 2, swapped_params, 2, i32_type),
            func_type);
  EXPECT_NE(clux_context_create_func_type(
                ctx, "C5", captures, 2, params, 2, i64_type),
            func_type);

  /* 非法入参：缺名、缺返回类型、捕获缺名缺类型、重名、参数缺类型 */
  EXPECT_EQ(clux_context_create_func_type(
                ctx, NULL, captures, 2, params, 2, i32_type),
            nullptr);
  EXPECT_EQ(
      clux_context_create_func_type(ctx, "E", captures, 2, params, 2, NULL),
      nullptr);
  clux_func_capture_t cap_bad[] = {
      {"a", i32_type},
      {"x", NULL},
  };
  EXPECT_EQ(clux_context_create_func_type(
                ctx, "Bad", cap_bad, 2, params, 2, i32_type),
            nullptr);
  clux_func_capture_t cap_dup[] = {
      {"a", i32_type},
      {"a", i64_type},
  };
  EXPECT_EQ(clux_context_create_func_type(
                ctx, "Dup", cap_dup, 2, params, 2, i32_type),
            nullptr);
  clux_type_t *bad_params[] = {i32_type, NULL};
  EXPECT_EQ(clux_context_create_func_type(
                ctx, "BadP", captures, 2, bad_params, 2, i32_type),
            nullptr);

  /* 零值创建：AST 指针 NULL，捕获字段清零；set_ast 挂接函数体 */
  static int dummy_body;
  clux_value_t *f = clux_context_create_func(ctx, func_type, "f", NULL);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(clux_value_get_type(f), func_type);
  EXPECT_EQ(clux_func_get_ast(f), nullptr);
  EXPECT_EQ(clux_context_func_set_ast(ctx, f, &dummy_body, NULL), f);
  EXPECT_EQ(clux_func_get_ast(f), &dummy_body);

  /* 捕获读取：独立 API 返回双向 view，写穿 view */
  clux_value_t *cap_a = clux_context_func_capture(ctx, f, "a", NULL);
  ASSERT_NE(cap_a, nullptr);
  EXPECT_EQ(clux_value_get_type(cap_a), i32_type);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, cap_a, five, NULL), cap_a);
  EXPECT_EQ(read_i32(clux_context_func_capture(ctx, f, "a", NULL)), 5);
  clux_value_t *cap_b = clux_context_func_capture(ctx, f, "b", NULL);
  ASSERT_NE(cap_b, nullptr);
  clux_value_t *seven = clux_context_create_i64(ctx, 7, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, cap_b, seven, NULL), cap_b);
  EXPECT_EQ(read_i64(clux_context_func_capture(ctx, f, "b", NULL)), 7);

  /* 未知捕获名 */
  clux_value_t *missing = clux_context_func_capture(ctx, f, "x", NULL);
  ASSERT_NE(missing, nullptr);
  EXPECT_STREQ(error_message_of(missing),
               "func 'func|a:i32,b:i64|(i32,i32)->i32' has no capture 'x'");

  /* invoke：argc 不匹配 */
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *args1[] = {one};
  clux_value_t *too_few = clux_context_invoke(ctx, f, 1, args1, NULL);
  ASSERT_NE(too_few, nullptr);
  EXPECT_STREQ(error_message_of(too_few),
               "function expects 2 arguments, got 1");
  clux_value_t *too_many = clux_context_invoke(ctx, f, 0, NULL, NULL);
  ASSERT_NE(too_many, nullptr);
  EXPECT_STREQ(error_message_of(too_many),
               "function expects 2 arguments, got 0");
  /* invoke：实参类型不匹配 */
  clux_value_t *args_bad[] = {seven, one};
  clux_value_t *bad_arg = clux_context_invoke(ctx, f, 2, args_bad, NULL);
  ASSERT_NE(bad_arg, nullptr);
  EXPECT_STREQ(error_message_of(bad_arg),
               "function argument 0 requires type 'i32', got 'i64'");
  /* invoke：error 实参自传播 */
  clux_value_t *err = clux_context_func_capture(ctx, f, "x", NULL);
  clux_value_t *args_err[] = {err, one};
  EXPECT_EQ(clux_context_invoke(ctx, f, 2, args_err, NULL), err);
  /* invoke：shadow 实参产出返回类型 shadow */
  clux_value_t *shadow_i32 = clux_context_create_shadow_value(ctx, i32_type);
  clux_value_t *args_shadow[] = {shadow_i32, one};
  clux_value_t *shadow_result =
      clux_context_invoke(ctx, f, 2, args_shadow, NULL);
  ASSERT_NE(shadow_result, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_result));
  EXPECT_EQ(clux_value_get_type(shadow_result), i32_type);
  /* invoke：校验通过的真函数无可执行环境是运行期 error */
  clux_value_t *args_ok[] = {one, five};
  clux_value_t *no_env = clux_context_invoke(ctx, f, 2, args_ok, NULL);
  ASSERT_NE(no_env, nullptr);
  EXPECT_STREQ(error_message_of(no_env),
               "function call has no executable environment attached");

  /* shadow：func_capture 产出捕获 shadow，invoke 产出返回类型 shadow，
     set_ast 原样返回 */
  clux_value_t *shadow_func = clux_context_create_shadow_value(ctx, func_type);
  clux_value_t *shadow_cap =
      clux_context_func_capture(ctx, shadow_func, "a", NULL);
  ASSERT_NE(shadow_cap, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_cap));
  EXPECT_EQ(clux_value_get_type(shadow_cap), i32_type);
  clux_value_t *shadow_call =
      clux_context_invoke(ctx, shadow_func, 2, args_ok, NULL);
  ASSERT_NE(shadow_call, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_call));
  EXPECT_EQ(clux_value_get_type(shadow_call), i32_type);
  EXPECT_EQ(clux_context_func_set_ast(ctx, shadow_func, &dummy_body, NULL),
            shadow_func);

  /* 非 func 类型的兜底报错 */
  clux_value_t *plain = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *bad_create =
      clux_context_create_func(ctx, i32_type, NULL, NULL);
  ASSERT_NE(bad_create, nullptr);
  EXPECT_STREQ(error_message_of(bad_create),
               "function value creation requires a func type");
  clux_value_t *bad_capture = clux_context_func_capture(ctx, plain, "a", NULL);
  ASSERT_NE(bad_capture, nullptr);
  EXPECT_STREQ(error_message_of(bad_capture),
               "capture access requires a func value, got 'i32'");
  clux_value_t *bad_ast =
      clux_context_func_set_ast(ctx, plain, &dummy_body, NULL);
  ASSERT_NE(bad_ast, nullptr);
  EXPECT_STREQ(error_message_of(bad_ast),
               "setting a function body requires a func value, got 'i32'");
  clux_value_t *bad_invoke = clux_context_invoke(ctx, plain, 0, NULL, NULL);
  ASSERT_NE(bad_invoke, nullptr);
  EXPECT_STREQ(error_message_of(bad_invoke),
               "invoke requires a function value, got 'i32'");

  /* eq/ne：函数身份是 AST 指针，捕获环境按字段递归比较 */
  clux_value_t *g = clux_context_create_func(ctx, func_type, "g", NULL);
  ASSERT_NE(g, nullptr);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, f, g, NULL)));
  EXPECT_EQ(clux_context_func_set_ast(ctx, g, &dummy_body, NULL), g);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, f, g, NULL)));
  clux_value_t *g_cap_a = clux_context_func_capture(ctx, g, "a", NULL);
  EXPECT_EQ(clux_context_assign(ctx, g_cap_a, five, NULL), g_cap_a);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, f, g, NULL)));
  clux_value_t *g_cap_b = clux_context_func_capture(ctx, g, "b", NULL);
  EXPECT_EQ(clux_context_assign(ctx, g_cap_b, seven, NULL), g_cap_b);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_EQ, f, g, NULL)));
  clux_value_t *eight = clux_context_create_i64(ctx, 8, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, g_cap_b, eight, NULL), g_cap_b);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, f, g, NULL)));
  clux_value_t *mixed = clux_context_binary(ctx, CLUX_OP_EQ, f, plain, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_STREQ(error_message_of(mixed),
               "operator '==' requires operands of the same type");
  /* 跨签名：无捕获函数与带捕获函数不可比（同类型比较才有效） */
  clux_value_t *plain_func =
      clux_context_create_func(ctx, plain_type, NULL, NULL);
  ASSERT_NE(plain_func, nullptr);
  clux_value_t *cross =
      clux_context_binary(ctx, CLUX_OP_EQ, f, plain_func, NULL);
  ASSERT_NE(cross, nullptr);
  EXPECT_STREQ(error_message_of(cross),
               "operator '==' requires operands of the same type");

  /* 同类型 assign 整体拷贝（AST 指针+捕获字段）+ clone */
  EXPECT_EQ(clux_func_get_ast(clux_context_assign(ctx, g, f, NULL)),
            &dummy_body);
  EXPECT_EQ(read_i64(clux_context_func_capture(ctx, g, "b", NULL)), 7);
  clux_value_t *copy = clux_context_clone(ctx, f, NULL);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(clux_func_get_ast(copy), &dummy_body);
  EXPECT_EQ(read_i32(clux_context_func_capture(ctx, copy, "a", NULL)), 5);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_EQ, copy, f, NULL)));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StructTypeFieldsAndEquality) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);

  /* 创建：字段按声明序 C 布局平铺
     （a 偏移 0，b 偏移 round_up(4, 8) = 8，size = round_up(16, 8) = 16） */
  clux_struct_field_t fields[] = {
      {"a", i32_type},
      {"b", i64_type},
  };
  clux_struct_decl_t decl = {fields, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *point_type =
      clux_context_create_struct_type(ctx, "Point", &decl);
  ASSERT_NE(point_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(point_type), CLUX_TYPE_STRUCT);
  EXPECT_STREQ(clux_type_get_name(point_type), "Point");
  EXPECT_EQ(clux_type_get_size(point_type), 16u);
  EXPECT_EQ(clux_type_get_align(point_type), 8u);
  EXPECT_EQ(clux_struct_field_count(point_type), 2u);
  EXPECT_EQ(clux_struct_get_field(point_type, 1)->type, i64_type);
  EXPECT_EQ(clux_struct_get_field(point_type, 2), nullptr);
  EXPECT_NE(clux_struct_find_field(point_type, "b"), nullptr);
  EXPECT_EQ(clux_struct_find_field(point_type, "x"), nullptr);
  EXPECT_EQ(clux_struct_field_offset(point_type, 0), 0u);
  EXPECT_EQ(clux_struct_field_offset(point_type, 1), 8u);

  /* 结构去重：四张声明表一致即恒等返回 */
  clux_struct_field_t same[] = {
      {"a", i32_type},
      {"b", i64_type},
  };
  clux_struct_decl_t same_decl = {same, 2, NULL, 0, NULL, 0, NULL, 0};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "Other", &same_decl),
            point_type);
  clux_struct_field_t swapped[] = {
      {"b", i64_type},
      {"a", i32_type},
  };
  clux_struct_decl_t swapped_decl = {swapped, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *swapped_type =
      clux_context_create_struct_type(ctx, "Swapped", &swapped_decl);
  ASSERT_NE(swapped_type, nullptr);
  EXPECT_NE(swapped_type, point_type);
  EXPECT_FALSE(clux_type_equal(point_type, swapped_type));

  /* 非法入参：缺名、空字段表、空声明体、字段缺类型、重名 */
  EXPECT_EQ(clux_context_create_struct_type(ctx, NULL, &decl), nullptr);
  clux_struct_decl_t empty_decl = {fields, 0, NULL, 0, NULL, 0, NULL, 0};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "E", &empty_decl), nullptr);
  EXPECT_EQ(clux_context_create_struct_type(ctx, "E", NULL), nullptr);
  clux_struct_field_t bad[] = {
      {"a", i32_type},
      {"x", NULL},
  };
  clux_struct_decl_t bad_decl = {bad, 2, NULL, 0, NULL, 0, NULL, 0};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "Bad", &bad_decl), nullptr);
  clux_struct_field_t dup[] = {
      {"a", i32_type},
      {"a", i64_type},
  };
  clux_struct_decl_t dup_decl = {dup, 2, NULL, 0, NULL, 0, NULL, 0};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "Dup", &dup_decl), nullptr);

  /* 零值创建 + 字段读写（通用 field，写穿 view，全部字段同时有效） */
  clux_value_t *p = clux_context_create_struct(ctx, point_type, "p", NULL);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(clux_value_get_type(p), point_type);
  clux_value_t *a_field = clux_context_field(ctx, p, "a", NULL);
  ASSERT_NE(a_field, nullptr);
  EXPECT_EQ(clux_value_get_type(a_field), i32_type);
  EXPECT_EQ(read_i32(a_field), 0);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, a_field, five, NULL), a_field);
  clux_value_t *b_field = clux_context_field(ctx, p, "b", NULL);
  ASSERT_NE(b_field, nullptr);
  clux_value_t *seven = clux_context_create_i64(ctx, 7, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, b_field, seven, NULL), b_field);
  EXPECT_EQ(read_i32(clux_context_field(ctx, p, "a", NULL)), 5);
  EXPECT_EQ(read_i64(clux_context_field(ctx, p, "b", NULL)), 7);

  /* 未知字段 */
  clux_value_t *missing = clux_context_field(ctx, p, "x", NULL);
  ASSERT_NE(missing, nullptr);
  EXPECT_STREQ(error_message_of(missing), "struct 'Point' has no field 'x'");

  /* shadow：field 产出字段 shadow，eq 产出 bool shadow */
  clux_value_t *shadow_struct =
      clux_context_create_shadow_value(ctx, point_type);
  clux_value_t *shadow_field =
      clux_context_field(ctx, shadow_struct, "a", NULL);
  ASSERT_NE(shadow_field, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_field));
  EXPECT_EQ(clux_value_get_type(shadow_field), i32_type);
  clux_value_t *shadow_eq =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow_struct, p, NULL);
  ASSERT_NE(shadow_eq, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_eq));
  EXPECT_EQ(clux_value_get_type(shadow_eq),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));

  /* eq/ne 逐字段递归（白皮书 §5.5） */
  clux_value_t *q = clux_context_create_struct(ctx, point_type, NULL, NULL);
  ASSERT_NE(q, nullptr);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, p, q, NULL)));
  clux_value_t *q_a = clux_context_field(ctx, q, "a", NULL);
  ASSERT_NE(q_a, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, q_a, five, NULL), q_a);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_NE, p, q, NULL)));
  clux_value_t *q_b = clux_context_field(ctx, q, "b", NULL);
  ASSERT_NE(q_b, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, q_b, seven, NULL), q_b);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_EQ, p, q, NULL)));
  clux_value_t *other =
      clux_context_create_struct(ctx, swapped_type, NULL, NULL);
  ASSERT_NE(other, nullptr);
  clux_value_t *mixed = clux_context_binary(ctx, CLUX_OP_EQ, p, other, NULL);
  ASSERT_NE(mixed, nullptr);
  EXPECT_STREQ(error_message_of(mixed),
               "operator '==' requires operands of the same type");
  clux_value_t *plain = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *mixed_plain =
      clux_context_binary(ctx, CLUX_OP_EQ, p, plain, NULL);
  ASSERT_NE(mixed_plain, nullptr);
  EXPECT_STREQ(error_message_of(mixed_plain),
               "operator '==' requires operands of the same type");

  /* 非 struct 类型的兜底报错 */
  clux_value_t *bad_create =
      clux_context_create_struct(ctx, i32_type, NULL, NULL);
  ASSERT_NE(bad_create, nullptr);
  EXPECT_STREQ(error_message_of(bad_create),
               "struct value creation requires a struct type");
  clux_value_t *bad_field = clux_context_field(ctx, plain, "a", NULL);
  ASSERT_NE(bad_field, nullptr);
  EXPECT_STREQ(error_message_of(bad_field),
               "type 'i32' does not support field access");

  /* 同类型 assign 整体拷贝 + clone */
  EXPECT_EQ(read_i64(clux_context_field(
                ctx, clux_context_assign(ctx, q, p, NULL), "b", NULL)),
            7);
  clux_value_t *copy = clux_context_clone(ctx, p, NULL);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(read_i32(clux_context_field(ctx, copy, "a", NULL)), 5);
  EXPECT_EQ(read_i64(clux_context_field(ctx, copy, "b", NULL)), 7);
  EXPECT_TRUE(read_bool(clux_context_binary(ctx, CLUX_OP_EQ, copy, p, NULL)));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StructAttributesMethodsSubtypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  /* method 本体：func(i32, i32) -> i32，挂 AST */
  clux_type_t *param_types[] = {i32_type, i32_type};
  clux_type_t *method_fn_type = clux_context_create_func_type(
      ctx, "func(i32,i32)->i32", NULL, 0, param_types, 2, i32_type);
  ASSERT_NE(method_fn_type, nullptr);
  static int dummy_body;
  clux_value_t *scale =
      clux_context_create_func(ctx, method_fn_type, "scale", NULL);
  ASSERT_NE(scale, nullptr);
  ASSERT_EQ(clux_context_func_set_ast(ctx, scale, &dummy_body, NULL), scale);
  /* 静态变量 attr（var count: i32 = 0） */
  clux_value_t *count = clux_context_create_i32(ctx, 0, "count", NULL);
  ASSERT_NE(count, nullptr);
  /* var fn = func(...){...} 不是 method，只是 attr */
  clux_value_t *plain_fn =
      clux_context_create_func(ctx, method_fn_type, "plain_fn", NULL);
  ASSERT_NE(plain_fn, nullptr);

  clux_struct_field_t fields[] = {{"x", i32_type}};
  clux_struct_attr_t attrs[] = {
      {"scale", scale},
      {"count", count},
      {"plain_fn", plain_fn},
  };
  const char *methods[] = {"scale"};
  clux_struct_subtype_t subtypes[] = {{"Tag", i32_type}};
  clux_struct_decl_t decl = {fields, 1, attrs, 3, methods, 1, subtypes, 1};
  clux_type_t *box_type = clux_context_create_struct_type(ctx, "Box", &decl);
  ASSERT_NE(box_type, nullptr);

  /* Data 布局只由字段决定（attribute/subtype 不占实例空间） */
  EXPECT_EQ(clux_type_get_size(box_type), 4u);
  EXPECT_EQ(clux_type_get_align(box_type), 4u);
  EXPECT_EQ(clux_struct_attr_count(box_type), 3u);
  EXPECT_EQ(clux_struct_get_attr(box_type, 1)->value, count);
  EXPECT_EQ(clux_struct_get_attr(box_type, 3), nullptr);
  EXPECT_NE(clux_struct_find_attr(box_type, "plain_fn"), nullptr);
  EXPECT_EQ(clux_struct_subtype_count(box_type), 1u);
  EXPECT_EQ(clux_struct_get_subtype(box_type, 1), nullptr);
  EXPECT_EQ(clux_struct_find_subtype(box_type, "Tag")->type, i32_type);

  /* method 判定看声明形式：名单里的名字才是 method */
  EXPECT_TRUE(clux_struct_is_method(box_type, "scale"));
  EXPECT_FALSE(clux_struct_is_method(box_type, "count"));
  EXPECT_FALSE(clux_struct_is_method(box_type, "plain_fn"));
  EXPECT_FALSE(clux_struct_is_method(box_type, "missing"));

  /* 读取分形态：attr/method 借用返回值本体，subtype 返回类型 */
  EXPECT_EQ(clux_context_struct_attr(ctx, box_type, "scale", NULL), scale);
  EXPECT_EQ(clux_context_struct_attr(ctx, box_type, "count", NULL), count);
  clux_value_t *missing_attr =
      clux_context_struct_attr(ctx, box_type, "y", NULL);
  ASSERT_NE(missing_attr, nullptr);
  EXPECT_STREQ(error_message_of(missing_attr),
               "struct 'Box' has no attribute 'y'");
  clux_value_t *bad_attr = clux_context_struct_attr(ctx, i32_type, "x", NULL);
  ASSERT_NE(bad_attr, nullptr);
  EXPECT_STREQ(error_message_of(bad_attr),
               "attribute access requires a struct type, got 'i32'");
  EXPECT_EQ(clux_context_struct_subtype(ctx, box_type, "Tag", NULL), i32_type);
  EXPECT_EQ(clux_context_struct_subtype(ctx, box_type, "Other", NULL), nullptr);
  EXPECT_EQ(clux_context_struct_subtype(ctx, i32_type, "Tag", NULL), nullptr);

  /* method 经 invoke 调用：实参按签名校验（receiver 作首参是语法糖
     a.scale(3) ⇒ T::scale(a, 3) 的展开约定，属 parser/编译层职责），
     真函数校验通过报无可执行环境（AST 执行由编译流水线后端接线） */
  clux_value_t *three = clux_context_create_i32(ctx, 3, NULL, NULL);
  clux_value_t *method_args[] = {three, three};
  clux_value_t *called = clux_context_invoke(
      ctx,
      clux_context_struct_attr(ctx, box_type, "scale", NULL),
      2,
      method_args,
      NULL);
  ASSERT_NE(called, nullptr);
  EXPECT_STREQ(error_message_of(called),
               "function call has no executable environment attached");
  /* shadow 方法产出返回类型 shadow */
  clux_value_t *shadow_fn =
      clux_context_create_shadow_value(ctx, method_fn_type);
  clux_value_t *shadow_call =
      clux_context_invoke(ctx, shadow_fn, 2, method_args, NULL);
  ASSERT_NE(shadow_call, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_call));
  EXPECT_EQ(clux_value_get_type(shadow_call), i32_type);

  /* 去重：四张声明表一致即恒等返回（attr 按名+值类型判等，值本体不参与） */
  clux_value_t *other_scale =
      clux_context_create_func(ctx, method_fn_type, NULL, NULL);
  clux_value_t *other_count = clux_context_create_i32(ctx, 9, NULL, NULL);
  clux_value_t *other_fn =
      clux_context_create_func(ctx, method_fn_type, NULL, NULL);
  clux_struct_attr_t same_attrs[] = {
      {"scale", other_scale},
      {"count", other_count},
      {"plain_fn", other_fn},
  };
  clux_struct_decl_t same_decl = {
      fields, 1, same_attrs, 3, methods, 1, subtypes, 1};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "Same", &same_decl), box_type);
  /* 差一个 attr 即不同 */
  clux_struct_decl_t less_attr_decl = {
      fields, 1, same_attrs, 2, methods, 1, subtypes, 1};
  clux_type_t *less_type =
      clux_context_create_struct_type(ctx, "Less", &less_attr_decl);
  ASSERT_NE(less_type, nullptr);
  EXPECT_NE(less_type, box_type);
  EXPECT_FALSE(clux_type_equal(box_type, less_type));
  /* method 名单差一项即不同（plain_fn 升 method） */
  const char *more_methods[] = {"scale", "plain_fn"};
  clux_struct_decl_t more_method_decl = {
      fields, 1, same_attrs, 3, more_methods, 2, subtypes, 1};
  clux_type_t *more_type =
      clux_context_create_struct_type(ctx, "More", &more_method_decl);
  ASSERT_NE(more_type, nullptr);
  EXPECT_NE(more_type, box_type);
  EXPECT_FALSE(clux_type_equal(box_type, more_type));
  /* subtype 差一项即不同 */
  clux_struct_decl_t no_subtype_decl = {
      fields, 1, same_attrs, 3, methods, 1, NULL, 0};
  clux_type_t *flat_type =
      clux_context_create_struct_type(ctx, "Flat", &no_subtype_decl);
  ASSERT_NE(flat_type, nullptr);
  EXPECT_NE(flat_type, box_type);
  EXPECT_FALSE(clux_type_equal(box_type, flat_type));

  /* 非法声明：method 不在 attribute 表、method 值非 func、attr 缺值、
     attr/method/subtype 重名、subtype 缺类型 */
  const char *ghost_methods[] = {"ghost"};
  clux_struct_decl_t ghost_decl = {
      fields, 1, attrs, 3, ghost_methods, 1, subtypes, 1};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "Ghost", &ghost_decl),
            nullptr);
  const char *count_methods[] = {"count"};
  clux_struct_decl_t not_fn_decl = {
      fields, 1, attrs, 3, count_methods, 1, subtypes, 1};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "NotFn", &not_fn_decl),
            nullptr);
  clux_struct_attr_t valueless[] = {{"scale", NULL}};
  clux_struct_decl_t valueless_decl = {
      fields, 1, valueless, 1, NULL, 0, NULL, 0};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "NoValue", &valueless_decl),
            nullptr);
  clux_struct_attr_t dup_attrs[] = {{"scale", scale}, {"scale", scale}};
  clux_struct_decl_t dup_attr_decl = {
      fields, 1, dup_attrs, 2, NULL, 0, NULL, 0};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "DupAttr", &dup_attr_decl),
            nullptr);
  const char *dup_methods[] = {"scale", "scale"};
  clux_struct_decl_t dup_method_decl = {
      fields, 1, attrs, 3, dup_methods, 2, subtypes, 1};
  EXPECT_EQ(clux_context_create_struct_type(ctx, "DupMethod", &dup_method_decl),
            nullptr);
  clux_struct_subtype_t bad_subtypes[] = {{"Tag", i32_type}, {"V", NULL}};
  clux_struct_decl_t bad_subtype_decl = {
      fields, 1, attrs, 3, methods, 1, bad_subtypes, 2};
  EXPECT_EQ(
      clux_context_create_struct_type(ctx, "BadSubtype", &bad_subtype_decl),
      nullptr);
  clux_struct_subtype_t dup_subtypes[] = {{"Tag", i32_type}, {"Tag", i32_type}};
  clux_struct_decl_t dup_subtype_decl = {
      fields, 1, attrs, 3, methods, 1, dup_subtypes, 2};
  EXPECT_EQ(
      clux_context_create_struct_type(ctx, "DupSubtype", &dup_subtype_decl),
      nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StructPointerCovariantImplicitCast) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_type_t *f32_type = clux_context_builtin_type(ctx, CLUX_TYPE_F32);

  /* Base{a,b} ← Derived{a,b,c} 字段前缀兼容；Unrelated 横跨不兼容 */
  clux_struct_field_t base_fields[] = {
      {"a", i32_type},
      {"b", i64_type},
  };
  clux_struct_decl_t base_decl = {base_fields, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *base_type =
      clux_context_create_struct_type(ctx, "Base", &base_decl);
  ASSERT_NE(base_type, nullptr);
  clux_struct_field_t derived_fields[] = {
      {"a", i32_type},
      {"b", i64_type},
      {"c", f32_type},
  };
  clux_struct_decl_t derived_decl = {
      derived_fields, 3, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *derived_type =
      clux_context_create_struct_type(ctx, "Derived", &derived_decl);
  ASSERT_NE(derived_type, nullptr);
  clux_struct_field_t unrelated_fields[] = {
      {"x", i32_type},
  };
  clux_struct_decl_t unrelated_decl = {
      unrelated_fields, 1, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *unrelated_type =
      clux_context_create_struct_type(ctx, "Unrelated", &unrelated_decl);
  ASSERT_NE(unrelated_type, nullptr);

  /* 类型级 extends：字段前缀兼容是单向的（向下兼容），自反成立 */
  EXPECT_TRUE(clux_type_extends(derived_type, base_type));
  EXPECT_FALSE(clux_type_extends(base_type, derived_type));
  EXPECT_TRUE(clux_type_extends(base_type, base_type));
  EXPECT_FALSE(clux_type_extends(base_type, unrelated_type));
  EXPECT_FALSE(clux_type_extends(derived_type, unrelated_type));
  /* attribute 不参与 extends（只看字段） */
  clux_value_t *tag = clux_context_create_i32(ctx, 1, "tag", NULL);
  ASSERT_NE(tag, nullptr);
  clux_struct_attr_t tagged_attrs[] = {{"tag", tag}};
  clux_struct_decl_t tagged_decl = {
      derived_fields, 3, tagged_attrs, 1, NULL, 0, NULL, 0};
  clux_type_t *tagged_type =
      clux_context_create_struct_type(ctx, "Tagged", &tagged_decl);
  ASSERT_NE(tagged_type, nullptr);
  EXPECT_TRUE(clux_type_extends(tagged_type, base_type));

  /* 指针协变接通：*Derived extends *Base，双重指针递归成立，反向拒绝 */
  clux_type_t *base_ptr_type = clux_context_get_ptr_type(ctx, base_type);
  clux_type_t *derived_ptr_type = clux_context_get_ptr_type(ctx, derived_type);
  clux_type_t *unrelated_ptr_type =
      clux_context_get_ptr_type(ctx, unrelated_type);
  EXPECT_STREQ(clux_type_get_name(base_ptr_type), "*Base");
  EXPECT_TRUE(clux_type_extends(derived_ptr_type, base_ptr_type));
  EXPECT_FALSE(clux_type_extends(base_ptr_type, derived_ptr_type));
  EXPECT_FALSE(clux_type_extends(derived_ptr_type, unrelated_ptr_type));
  EXPECT_TRUE(
      clux_type_extends(clux_context_get_ptr_type(ctx, derived_ptr_type),
                        clux_context_get_ptr_type(ctx, base_ptr_type)));

  /* 隐式转换：assign 异类型走 safe_cast，*Derived 值可写入 *Base 变量 */
  clux_value_t *d = clux_context_create_struct(ctx, derived_type, "d", NULL);
  ASSERT_NE(d, nullptr);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  ASSERT_NE(five, nullptr);
  clux_value_t *field_a = clux_context_field(ctx, d, "a", NULL);
  ASSERT_NE(field_a, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, field_a, five, NULL), field_a);
  clux_value_t *d_ptr = clux_context_ref(ctx, d, NULL);
  ASSERT_NE(d_ptr, nullptr);
  clux_value_t *base_slot =
      clux_context_create_ptr(ctx, base_ptr_type, 0, "base_slot", NULL);
  ASSERT_NE(base_slot, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, base_slot, d_ptr, NULL), base_slot);
  /* 转换后经 Base 视图读到同一实例 */
  EXPECT_EQ(read_i32(clux_context_field(
                ctx, clux_context_deref(ctx, base_slot, NULL), "a", NULL)),
            5);

  /* 显式 safe_cast：成功产出目标类型；shadow 产出目标类型 shadow */
  clux_value_t *up = clux_context_safe_cast(ctx, d_ptr, base_ptr_type, NULL);
  ASSERT_NE(up, nullptr);
  EXPECT_EQ(clux_value_get_type(up), base_ptr_type);
  EXPECT_EQ(read_i32(clux_context_field(
                ctx, clux_context_deref(ctx, up, NULL), "a", NULL)),
            5);
  clux_value_t *shadow_ptr =
      clux_context_create_shadow_value(ctx, derived_ptr_type);
  ASSERT_NE(shadow_ptr, nullptr);
  clux_value_t *shadow_up =
      clux_context_safe_cast(ctx, shadow_ptr, base_ptr_type, NULL);
  ASSERT_NE(shadow_up, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_up));
  EXPECT_EQ(clux_value_get_type(shadow_up), base_ptr_type);

  /* 反向与横跨拒绝：向下兼容是单向的 */
  clux_value_t *b = clux_context_create_struct(ctx, base_type, "b", NULL);
  ASSERT_NE(b, nullptr);
  clux_value_t *b_ptr = clux_context_ref(ctx, b, NULL);
  ASSERT_NE(b_ptr, nullptr);
  clux_value_t *down =
      clux_context_safe_cast(ctx, b_ptr, derived_ptr_type, NULL);
  ASSERT_NE(down, nullptr);
  EXPECT_STREQ(error_message_of(down),
               "cannot safely cast '*Base' to '*Derived'");
  clux_value_t *cross =
      clux_context_safe_cast(ctx, d_ptr, unrelated_ptr_type, NULL);
  ASSERT_NE(cross, nullptr);
  EXPECT_STREQ(error_message_of(cross),
               "cannot safely cast '*Derived' to '*Unrelated'");
  /* 隐式路径同样拒绝：*Base 值写入 *Derived 变量 */
  clux_value_t *derived_slot =
      clux_context_create_ptr(ctx, derived_ptr_type, 0, "derived_slot", NULL);
  ASSERT_NE(derived_slot, nullptr);
  clux_value_t *rev = clux_context_assign(ctx, derived_slot, b_ptr, NULL);
  ASSERT_NE(rev, nullptr);
  EXPECT_STREQ(error_message_of(rev),
               "cannot safely cast '*Base' to '*Derived'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, StructDuckTypeImplicitCast) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_type_t *f32_type = clux_context_builtin_type(ctx, CLUX_TYPE_F32);

  /* P 与 Q 字段表一致但 attribute 声明不同：不去重折叠为同一类型，
     鸭子判据（只看字段名+顺序+类型）双向成立 */
  clux_struct_field_t fields[] = {
      {"x", i32_type},
      {"y", i64_type},
  };
  clux_value_t *tag = clux_context_create_i32(ctx, 1, NULL, NULL);
  ASSERT_NE(tag, nullptr);
  clux_struct_attr_t attrs[] = {{"tag", tag}};
  clux_struct_decl_t p_decl = {fields, 2, attrs, 1, NULL, 0, NULL, 0};
  clux_type_t *p_type = clux_context_create_struct_type(ctx, "P", &p_decl);
  ASSERT_NE(p_type, nullptr);
  clux_struct_decl_t q_decl = {fields, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *q_type = clux_context_create_struct_type(ctx, "Q", &q_decl);
  ASSERT_NE(q_type, nullptr);
  EXPECT_NE(p_type, q_type);
  EXPECT_FALSE(clux_type_equal(p_type, q_type));

  /* 隐式互赋：P 值写入 Q 变量，数据穿透可读；反向同理（相互赋值） */
  clux_value_t *p = clux_context_create_struct(ctx, p_type, "p", NULL);
  ASSERT_NE(p, nullptr);
  clux_value_t *seven = clux_context_create_i32(ctx, 7, NULL, NULL);
  ASSERT_NE(seven, nullptr);
  clux_value_t *field_x = clux_context_field(ctx, p, "x", NULL);
  ASSERT_NE(field_x, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, field_x, seven, NULL), field_x);
  clux_value_t *q = clux_context_create_struct(ctx, q_type, "q", NULL);
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, q, p, NULL), q);
  EXPECT_EQ(read_i32(clux_context_field(ctx, q, "x", NULL)), 7);
  clux_value_t *p2 = clux_context_create_struct(ctx, p_type, "p2", NULL);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, p2, q, NULL), p2);
  EXPECT_EQ(read_i32(clux_context_field(ctx, p2, "x", NULL)), 7);

  /* 显式 safe_cast 产出目标类型；shadow 产出目标类型 shadow */
  clux_value_t *as_q = clux_context_safe_cast(ctx, p, q_type, NULL);
  ASSERT_NE(as_q, nullptr);
  EXPECT_EQ(clux_value_get_type(as_q), q_type);
  clux_value_t *shadow_p = clux_context_create_shadow_value(ctx, p_type);
  ASSERT_NE(shadow_p, nullptr);
  clux_value_t *shadow_q = clux_context_safe_cast(ctx, shadow_p, q_type, NULL);
  ASSERT_NE(shadow_q, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_q));
  EXPECT_EQ(clux_value_get_type(shadow_q), q_type);

  /* 拒绝：字段名不同、顺序不同、类型不同、字段数不同（前缀 extends
     成立但值语义要求字段表完全一致）、非 struct 目标 */
  clux_struct_field_t renamed[] = {
      {"z", i32_type},
      {"y", i64_type},
  };
  clux_struct_decl_t r_decl = {renamed, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *r_type = clux_context_create_struct_type(ctx, "R", &r_decl);
  ASSERT_NE(r_type, nullptr);
  clux_value_t *bad_name = clux_context_safe_cast(ctx, p, r_type, NULL);
  ASSERT_NE(bad_name, nullptr);
  EXPECT_STREQ(error_message_of(bad_name), "cannot safely cast 'P' to 'R'");
  clux_struct_field_t reordered[] = {
      {"y", i64_type},
      {"x", i32_type},
  };
  clux_struct_decl_t s_decl = {reordered, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *s_type = clux_context_create_struct_type(ctx, "S", &s_decl);
  ASSERT_NE(s_type, nullptr);
  clux_value_t *bad_order = clux_context_safe_cast(ctx, p, s_type, NULL);
  ASSERT_NE(bad_order, nullptr);
  EXPECT_STREQ(error_message_of(bad_order), "cannot safely cast 'P' to 'S'");
  clux_struct_field_t retyped[] = {
      {"x", f32_type},
      {"y", i64_type},
  };
  clux_struct_decl_t t_decl = {retyped, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *t_type = clux_context_create_struct_type(ctx, "T", &t_decl);
  ASSERT_NE(t_type, nullptr);
  clux_value_t *bad_type = clux_context_safe_cast(ctx, p, t_type, NULL);
  ASSERT_NE(bad_type, nullptr);
  EXPECT_STREQ(error_message_of(bad_type), "cannot safely cast 'P' to 'T'");
  clux_struct_field_t shorter[] = {
      {"x", i32_type},
  };
  clux_struct_decl_t u_decl = {shorter, 1, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *u_type = clux_context_create_struct_type(ctx, "U", &u_decl);
  ASSERT_NE(u_type, nullptr);
  /* P extends U（字段前缀）成立，但值互赋仍拒：U → P 方向 memcpy 按
     P 平铺会越界读 U 的内存——值语义要求字段表完全一致 */
  EXPECT_TRUE(clux_type_extends(p_type, u_type));
  clux_value_t *u = clux_context_create_struct(ctx, u_type, "u", NULL);
  ASSERT_NE(u, nullptr);
  clux_value_t *bad_count = clux_context_safe_cast(ctx, u, p_type, NULL);
  ASSERT_NE(bad_count, nullptr);
  EXPECT_STREQ(error_message_of(bad_count), "cannot safely cast 'U' to 'P'");
  clux_value_t *bad_kind = clux_context_safe_cast(ctx, p, i32_type, NULL);
  ASSERT_NE(bad_kind, nullptr);
  EXPECT_STREQ(error_message_of(bad_kind), "cannot safely cast 'P' to 'i32'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, TypeValueWrapsType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_type_t *type_type = clux_context_get_type_type(ctx);

  /* "type" 类型属性：vtable 只绑 eq 与 value_extends，其余槽位为空 */
  EXPECT_EQ(clux_type_get_kind(type_type), CLUX_TYPE_TYPE);
  EXPECT_STREQ(clux_type_get_name(type_type), "type");
  EXPECT_EQ(clux_type_get_size(type_type), sizeof(clux_type_t *));
  clux_vtable_t *vt = clux_type_get_vtable(type_type);
  ASSERT_NE(vt, nullptr);
  EXPECT_EQ(vt, get_clux_type_value_vtable());
  EXPECT_NE(vt->eq, nullptr);
  EXPECT_NE(vt->value_extends, nullptr);
  EXPECT_EQ(vt->ne, nullptr);
  EXPECT_EQ(vt->safe_cast, nullptr);
  EXPECT_EQ(vt->deref, nullptr);

  /* 创建：值类型是 "type"，data 指向被描述类型 */
  clux_value_t *t_i32 =
      clux_context_create_type_value(ctx, i32_type, "T", NULL);
  ASSERT_NE(t_i32, nullptr);
  EXPECT_EQ(clux_value_get_type(t_i32), type_type);
  void *raw = ptr_to_raw(clux_value_get_data(t_i32), 0);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(*(clux_type_t **)raw, i32_type);
  clux_value_t *bad_type_value =
      clux_context_create_type_value(ctx, NULL, "Bad", NULL);
  ASSERT_NE(bad_type_value, nullptr);
  EXPECT_STREQ(error_message_of(bad_type_value),
               "type value creation requires a type");

  /* eq 代理 clux_type_equal：语义判等（类型名与 attr 不参与） */
  clux_value_t *t_i32b =
      clux_context_create_type_value(ctx, i32_type, NULL, NULL);
  ASSERT_NE(t_i32b, nullptr);
  clux_value_t *t_i64 =
      clux_context_create_type_value(ctx, i64_type, NULL, NULL);
  ASSERT_NE(t_i64, nullptr);
  clux_value_t *same =
      clux_context_binary(ctx, CLUX_OP_EQ, t_i32, t_i32b, NULL);
  ASSERT_NE(same, nullptr);
  EXPECT_TRUE(*(bool *)ptr_to_raw(clux_value_get_data(same), 0));
  clux_value_t *diff = clux_context_binary(ctx, CLUX_OP_EQ, t_i32, t_i64, NULL);
  ASSERT_NE(diff, nullptr);
  EXPECT_FALSE(*(bool *)ptr_to_raw(clux_value_get_data(diff), 0));
  /* eq 代理的 type_equal 是四表判等：attr 声明不同即不同类型
     （鸭子互赋是 safe_cast 的职责，不影响类型身份） */
  clux_struct_field_t fields[] = {
      {"x", i32_type},
  };
  clux_value_t *tag = clux_context_create_i32(ctx, 1, NULL, NULL);
  ASSERT_NE(tag, nullptr);
  clux_struct_attr_t attrs[] = {{"tag", tag}};
  clux_struct_decl_t p_decl = {fields, 1, attrs, 1, NULL, 0, NULL, 0};
  clux_type_t *p_type = clux_context_create_struct_type(ctx, "P", &p_decl);
  ASSERT_NE(p_type, nullptr);
  clux_struct_decl_t q_decl = {fields, 1, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *q_type = clux_context_create_struct_type(ctx, "Q", &q_decl);
  ASSERT_NE(q_type, nullptr);
  EXPECT_NE(p_type, q_type);
  clux_value_t *t_p = clux_context_create_type_value(ctx, p_type, NULL, NULL);
  clux_value_t *t_q = clux_context_create_type_value(ctx, q_type, NULL, NULL);
  ASSERT_NE(t_p, nullptr);
  ASSERT_NE(t_q, nullptr);
  clux_value_t *duck_eq = clux_context_binary(ctx, CLUX_OP_EQ, t_p, t_q, NULL);
  ASSERT_NE(duck_eq, nullptr);
  EXPECT_FALSE(*(bool *)ptr_to_raw(clux_value_get_data(duck_eq), 0));
  /* shadow 产出 bool shadow；right 非 type value 报错；ne 不支持 */
  clux_value_t *shadow_t = clux_context_create_shadow_value(ctx, type_type);
  ASSERT_NE(shadow_t, nullptr);
  clux_value_t *shadow_eq =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow_t, t_i32, NULL);
  ASSERT_NE(shadow_eq, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_eq));
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);
  ASSERT_NE(one, nullptr);
  clux_value_t *bad_right =
      clux_context_binary(ctx, CLUX_OP_EQ, t_i32, one, NULL);
  ASSERT_NE(bad_right, nullptr);
  EXPECT_STREQ(error_message_of(bad_right),
               "operator '==' requires operands of the same type");
  clux_value_t *ne_result =
      clux_context_binary(ctx, CLUX_OP_NE, t_i32, t_i32b, NULL);
  ASSERT_NE(ne_result, nullptr);
  EXPECT_STREQ(error_message_of(ne_result),
               "type 'type' does not support operator '!='");

  /* value_extends 代理 clux_type_extends：*Derived extends *Base 成立，
     反向拒绝 */
  clux_type_t *f32_type = clux_context_builtin_type(ctx, CLUX_TYPE_F32);
  clux_struct_field_t base_fields[] = {
      {"a", i32_type},
      {"b", i64_type},
  };
  clux_struct_decl_t base_decl = {base_fields, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *base_type =
      clux_context_create_struct_type(ctx, "Base", &base_decl);
  ASSERT_NE(base_type, nullptr);
  clux_struct_field_t derived_fields[] = {
      {"a", i32_type},
      {"b", i64_type},
      {"c", f32_type},
  };
  clux_struct_decl_t derived_decl = {
      derived_fields, 3, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *derived_type =
      clux_context_create_struct_type(ctx, "Derived", &derived_decl);
  ASSERT_NE(derived_type, nullptr);
  clux_value_t *t_base_ptr = clux_context_create_type_value(
      ctx, clux_context_get_ptr_type(ctx, base_type), NULL, NULL);
  clux_value_t *t_derived_ptr = clux_context_create_type_value(
      ctx, clux_context_get_ptr_type(ctx, derived_type), NULL, NULL);
  ASSERT_NE(t_base_ptr, nullptr);
  ASSERT_NE(t_derived_ptr, nullptr);
  EXPECT_TRUE(vt->value_extends(t_derived_ptr, t_base_ptr));
  EXPECT_FALSE(vt->value_extends(t_base_ptr, t_derived_ptr));
  EXPECT_TRUE(vt->value_extends(t_i32, t_i32b));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, TypeValueVariableShareNamespace) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);

  /* 类型 T 以 type value 身份进词法作用域 */
  clux_value_t *t = clux_context_create_type_value(ctx, i32_type, "T", NULL);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(clux_value_get_type(t), clux_context_get_type_type(ctx));

  /* 变量与类型共用命名空间：同层变量 T 重名拒绝 */
  clux_value_t *dup_var = clux_context_create_i32(ctx, 5, "T", NULL);
  ASSERT_NE(dup_var, nullptr);
  EXPECT_STREQ(error_message_of(dup_var), "duplicate declaration 'T'");
  /* 同层类型 T 重名同样拒绝 */
  clux_value_t *dup_type =
      clux_context_create_type_value(ctx, i64_type, "T", NULL);
  ASSERT_NE(dup_type, nullptr);
  EXPECT_STREQ(error_message_of(dup_type), "duplicate declaration 'T'");

  /* 子层定义同名变量 T 遮罩外层类型：共用命名空间的遮罩规则自然生效 */
  clux_context_push_scope(ctx);
  clux_value_t *var = clux_context_create_i32(ctx, 7, "T", NULL);
  ASSERT_NE(var, nullptr);
  EXPECT_NE(clux_value_get_type(var), clux_context_get_type_type(ctx));
  /* 子层内再建类型 T：同层重名依旧拒绝 */
  clux_value_t *inner_dup =
      clux_context_create_type_value(ctx, i64_type, "T", NULL);
  ASSERT_NE(inner_dup, nullptr);
  EXPECT_STREQ(error_message_of(inner_dup), "duplicate declaration 'T'");
  clux_context_pop_scope(ctx);

  /* 弹出子层后外层类型 T 仍然存活：同名再定义依旧拒绝 */
  clux_value_t *after = clux_context_create_i32(ctx, 9, "T", NULL);
  ASSERT_NE(after, nullptr);
  EXPECT_STREQ(error_message_of(after), "duplicate declaration 'T'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, TemplateValueWrapsAst) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *type_tmpl = clux_context_get_type_template_type(ctx);
  clux_type_t *func_tmpl = clux_context_get_func_template_type(ctx);

  /* 两种内建模板类型：kind/size 正确，vtable 完全留空 */
  EXPECT_EQ(clux_type_get_kind(type_tmpl), CLUX_TYPE_TYPE_TEMPLATE);
  EXPECT_EQ(clux_type_get_kind(func_tmpl), CLUX_TYPE_FUNC_TEMPLATE);
  EXPECT_STREQ(clux_type_get_name(type_tmpl), "type_template");
  EXPECT_STREQ(clux_type_get_name(func_tmpl), "func_template");
  EXPECT_EQ(clux_type_get_size(type_tmpl), sizeof(void *));
  EXPECT_EQ(clux_type_get_size(func_tmpl), sizeof(void *));
  clux_vtable_t *tvt = clux_type_get_vtable(type_tmpl);
  clux_vtable_t *fvt = clux_type_get_vtable(func_tmpl);
  ASSERT_NE(tvt, nullptr);
  ASSERT_NE(fvt, nullptr);
  EXPECT_EQ(tvt, get_clux_type_template_vtable());
  EXPECT_EQ(fvt, get_clux_func_template_vtable());
  EXPECT_NE(tvt, fvt);
  EXPECT_EQ(tvt->add, nullptr);
  EXPECT_EQ(tvt->eq, nullptr);
  EXPECT_EQ(tvt->ne, nullptr);
  EXPECT_EQ(tvt->safe_cast, nullptr);
  EXPECT_EQ(tvt->deref, nullptr);
  EXPECT_EQ(tvt->index, nullptr);
  EXPECT_EQ(tvt->field, nullptr);
  EXPECT_EQ(tvt->invoke, nullptr);
  EXPECT_EQ(tvt->type_equal, nullptr);
  EXPECT_EQ(tvt->type_extends, nullptr);
  EXPECT_EQ(tvt->value_extends, nullptr);
  EXPECT_EQ(fvt->eq, nullptr);
  EXPECT_EQ(fvt->safe_cast, nullptr);

  /* 模板值：data 只存 AST 指针，ast 非空是硬约束 */
  int fake_ast = 0;
  void *ast = &fake_ast;
  clux_value_t *tt = clux_context_create_type_template(ctx, ast, "Box", NULL);
  ASSERT_NE(tt, nullptr);
  EXPECT_EQ(clux_value_get_type(tt), type_tmpl);
  void *raw = ptr_to_raw(clux_value_get_data(tt), 0);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(*(void **)raw, ast);
  clux_value_t *ft = clux_context_create_func_template(ctx, ast, "pick", NULL);
  ASSERT_NE(ft, nullptr);
  EXPECT_EQ(clux_value_get_type(ft), func_tmpl);

  /* 取回 AST（实例化通道）：两种模板都取回；shadow 与非法输入返回 NULL */
  EXPECT_EQ(clux_context_template_ast(tt), ast);
  EXPECT_EQ(clux_context_template_ast(ft), ast);
  clux_value_t *shadow_tmpl = clux_context_create_shadow_value(ctx, type_tmpl);
  ASSERT_NE(shadow_tmpl, nullptr);
  EXPECT_EQ(clux_context_template_ast(shadow_tmpl), nullptr);
  clux_value_t *not_template = clux_context_create_i32(ctx, 1, NULL, NULL);
  ASSERT_NE(not_template, nullptr);
  EXPECT_EQ(clux_context_template_ast(not_template), nullptr);
  EXPECT_EQ(clux_context_template_ast(nullptr), nullptr);

  /* ast 为空返回 error；两种模板共用词法命名空间（同层重名拒绝） */
  clux_value_t *bad = clux_context_create_type_template(ctx, NULL, "Bad", NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_STREQ(error_message_of(bad), "template creation requires an AST node");
  clux_value_t *dup = clux_context_create_func_template(ctx, ast, "Box", NULL);
  ASSERT_NE(dup, nullptr);
  EXPECT_STREQ(error_message_of(dup), "duplicate declaration 'Box'");

  /* vtable 留空：eq 等运算走 context 兜底 */
  clux_value_t *eq_result = clux_context_binary(ctx, CLUX_OP_EQ, tt, tt, NULL);
  ASSERT_NE(eq_result, nullptr);
  EXPECT_STREQ(error_message_of(eq_result),
               "type 'type_template' does not support operator '=='");

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
  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
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
  clux_value_t *deref = clux_context_deref(ctx, ptr, NULL);
  ASSERT_NE(deref, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, deref, right, NULL), deref);
  clux_value_t *same = clux_context_binary(ctx, CLUX_OP_EQ, ptr, ptr, NULL);
  ASSERT_NE(same, nullptr);
  clux_value_t *ptr_shadow =
      clux_context_ref(ctx,
                       clux_context_create_shadow_value(
                           ctx, clux_context_builtin_type(ctx, CLUX_TYPE_STR)),
                       NULL);
  ASSERT_NE(ptr_shadow, nullptr);
  clux_type_t *const_type = clux_type_get_qualified_type(
      clux_context_builtin_type(ctx, CLUX_TYPE_I32), CLUX_TYPE_CONST);
  clux_type_t *volatile_const_type =
      clux_type_get_qualified_type(const_type, CLUX_TYPE_VOLATILE);
  ASSERT_NE(volatile_const_type, nullptr);
  clux_value_t *qualified_read = clux_context_deref(
      ctx,
      clux_context_create_ptr(ctx,
                              clux_context_get_ptr_type(ctx, const_type),
                              clux_value_get_data(left),
                              NULL,
                              NULL),
      NULL);
  ASSERT_NE(qualified_read, nullptr);
  clux_type_t *slice_type = clux_context_get_slice_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *slice = clux_context_create_slice(
      ctx, slice_type, clux_value_get_data(left), 4, NULL, NULL);
  ASSERT_NE(slice, nullptr);
  ASSERT_NE(clux_context_slice_len(ctx, slice, NULL), nullptr);
  ASSERT_NE(clux_context_binary(ctx, CLUX_OP_EQ, slice, slice, NULL), nullptr);
  ASSERT_NE(clux_context_clone(ctx, slice, NULL), nullptr);
  clux_type_t *array_type = clux_context_get_array_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), 3);
  clux_value_t *array = clux_context_create_array(ctx, array_type, NULL, NULL);
  ASSERT_NE(array, nullptr);
  clux_value_t *array_zero = clux_context_index(
      ctx, array, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(array_zero, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, array_zero, left, NULL), array_zero);
  ASSERT_NE(clux_context_binary(ctx, CLUX_OP_EQ, array, array, NULL), nullptr);
  ASSERT_NE(clux_context_clone(ctx, array, NULL), nullptr);
  clux_value_t *some = clux_context_create_optional_some(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), left, NULL, NULL);
  ASSERT_NE(some, nullptr);
  ASSERT_NE(clux_context_take(ctx, some, NULL), nullptr);
  ASSERT_NE(clux_context_is_none(ctx, some, NULL), nullptr);
  clux_value_t *none = clux_context_create_optional_none(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), NULL, NULL);
  ASSERT_NE(none, nullptr);
  ASSERT_NE(clux_context_is_none(ctx, none, NULL), nullptr);
  EXPECT_EQ(clux_context_assign(ctx, none, some, NULL), none);
  ASSERT_NE(clux_context_clone(ctx, some, NULL), nullptr);
  ASSERT_NE(clux_context_binary(ctx, CLUX_OP_EQ, some, some, NULL), nullptr);
  clux_cunion_member_t cunion_members[] = {
      {"i", clux_context_builtin_type(ctx, CLUX_TYPE_I32)},
      {"l", clux_context_builtin_type(ctx, CLUX_TYPE_I64)},
  };
  clux_type_t *cunion_type =
      clux_context_create_cunion_type(ctx, "Boxed", cunion_members, 2);
  ASSERT_NE(cunion_type, nullptr);
  clux_value_t *boxed =
      clux_context_create_cunion(ctx, cunion_type, NULL, NULL);
  ASSERT_NE(boxed, nullptr);
  clux_value_t *boxed_i = clux_context_field(ctx, boxed, "i", NULL);
  ASSERT_NE(boxed_i, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, boxed_i, left, NULL), boxed_i);
  ASSERT_NE(clux_context_field(ctx, boxed, "x", NULL), nullptr);
  ASSERT_NE(clux_context_clone(ctx, boxed, NULL), nullptr);
  ASSERT_NE(clux_context_binary(ctx, CLUX_OP_EQ, boxed, boxed, NULL), nullptr);
  clux_union_member_t union_members[] = {
      {"i", clux_context_builtin_type(ctx, CLUX_TYPE_I32)},
      {"l", clux_context_builtin_type(ctx, CLUX_TYPE_I64)},
  };
  clux_type_t *union_type =
      clux_context_create_union_type(ctx, "Tagged", union_members, 2);
  ASSERT_NE(union_type, nullptr);
  clux_value_t *tagged = clux_context_create_union(ctx, union_type, NULL, NULL);
  ASSERT_NE(tagged, nullptr);
  ASSERT_NE(clux_context_is_member(ctx, tagged, "i", NULL), nullptr);
  ASSERT_NE(clux_context_union_set_member(ctx, tagged, "l", NULL), nullptr);
  ASSERT_NE(clux_context_field(ctx, tagged, "l", NULL), nullptr);
  ASSERT_NE(clux_context_field(ctx, tagged, "x", NULL), nullptr);
  ASSERT_NE(clux_context_is_member(ctx, tagged, "x", NULL), nullptr);
  ASSERT_NE(clux_context_clone(ctx, tagged, NULL), nullptr);
  ASSERT_NE(clux_context_binary(ctx, CLUX_OP_EQ, tagged, tagged, NULL),
            nullptr);
  clux_func_capture_t func_captures[] = {
      {"a", clux_context_builtin_type(ctx, CLUX_TYPE_I32)},
      {"b", clux_context_builtin_type(ctx, CLUX_TYPE_I64)},
  };
  clux_type_t *func_params[] = {clux_context_builtin_type(ctx, CLUX_TYPE_I32),
                                clux_context_builtin_type(ctx, CLUX_TYPE_I32)};
  clux_type_t *func_type = clux_context_create_func_type(
      ctx,
      "func|a:i32,b:i64|(i32,i32)->i32",
      func_captures,
      2,
      func_params,
      2,
      clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  ASSERT_NE(func_type, nullptr);
  clux_value_t *func = clux_context_create_func(ctx, func_type, NULL, NULL);
  ASSERT_NE(func, nullptr);
  ASSERT_NE(clux_context_func_set_ast(ctx, func, (void *)&func_type, NULL),
            nullptr);
  ASSERT_NE(clux_context_func_capture(ctx, func, "a", NULL), nullptr);
  ASSERT_NE(clux_context_func_capture(ctx, func, "x", NULL), nullptr);
  clux_value_t *func_args[] = {left, left};
  ASSERT_NE(clux_context_invoke(ctx, func, 2, func_args, NULL), nullptr);
  ASSERT_NE(clux_context_invoke(ctx, func, 1, func_args, NULL), nullptr);
  ASSERT_NE(clux_context_clone(ctx, func, NULL), nullptr);
  ASSERT_NE(clux_context_binary(ctx, CLUX_OP_EQ, func, func, NULL), nullptr);
  clux_struct_field_t struct_fields[] = {
      {"a", clux_context_builtin_type(ctx, CLUX_TYPE_I32)},
      {"b", clux_context_builtin_type(ctx, CLUX_TYPE_I64)},
  };
  clux_struct_decl_t struct_decl = {
      struct_fields, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *struct_type =
      clux_context_create_struct_type(ctx, "Point", &struct_decl);
  ASSERT_NE(struct_type, nullptr);
  clux_value_t *point =
      clux_context_create_struct(ctx, struct_type, NULL, NULL);
  ASSERT_NE(point, nullptr);
  ASSERT_NE(clux_context_field(ctx, point, "a", NULL), nullptr);
  ASSERT_NE(clux_context_field(ctx, point, "x", NULL), nullptr);
  ASSERT_NE(clux_context_clone(ctx, point, NULL), nullptr);
  ASSERT_NE(clux_context_binary(ctx, CLUX_OP_EQ, point, point, NULL), nullptr);
  clux_struct_attr_t membered_attrs[] = {
      {"m",
       clux_context_create_func(
           ctx,
           clux_context_create_func_type(
               ctx,
               "fn",
               NULL,
               0,
               NULL,
               0,
               clux_context_builtin_type(ctx, CLUX_TYPE_I32)),
           NULL,
           NULL)},
  };
  const char *membered_methods[] = {"m"};
  clux_struct_subtype_t membered_subtypes[] = {
      {"T", clux_context_builtin_type(ctx, CLUX_TYPE_I32)}};
  clux_struct_decl_t membered_decl = {struct_fields,
                                      2,
                                      membered_attrs,
                                      1,
                                      membered_methods,
                                      1,
                                      membered_subtypes,
                                      1};
  clux_type_t *membered_type =
      clux_context_create_struct_type(ctx, "Membered", &membered_decl);
  ASSERT_NE(membered_type, nullptr);
  ASSERT_NE(clux_context_struct_attr(ctx, membered_type, "m", NULL), nullptr);
  ASSERT_NE(clux_context_struct_attr(ctx, membered_type, "x", NULL), nullptr);
  ASSERT_NE(clux_context_struct_subtype(ctx, membered_type, "T", NULL),
            nullptr);
  EXPECT_EQ(clux_context_struct_subtype(ctx, membered_type, "U", NULL),
            nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}

/* ---- TDZ（temporal dead zone）：value 标记 + consume + assign 清除 ---- */

/* 场景: 新值构造恒非 TDZ；set_tdz 置位/复位生命周期（含 shadow 与 error 值）
   预期: create 后 is_tdz 为 false，set true 后为 true，set false 复位 */
TEST(EngineTest, TdzFlagLifecycleOnValues) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *i32v = clux_context_create_i32(ctx, 10, NULL, NULL);
  ASSERT_NE(i32v, nullptr);
  EXPECT_FALSE(clux_value_is_tdz(i32v)) << "new value must not start in TDZ";
  clux_value_set_tdz(i32v, true);
  EXPECT_TRUE(clux_value_is_tdz(i32v));
  clux_value_set_tdz(i32v, false);
  EXPECT_FALSE(clux_value_is_tdz(i32v));

  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  ASSERT_NE(shadow, nullptr);
  EXPECT_FALSE(clux_value_is_tdz(shadow));
  clux_value_set_tdz(shadow, true);
  EXPECT_TRUE(clux_value_is_tdz(shadow));

  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "probe");
  ASSERT_NE(err, nullptr);
  EXPECT_FALSE(clux_value_is_tdz(err));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: consume_value 消费 TDZ 值
   预期: 返回 error 值，消息精确为 "use of a value before initialization" */
TEST(EngineTest, ConsumeValueRejectsTdzWithExactMessage) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *x = clux_context_create_i32(ctx, 1, "tdz_x", NULL);
  ASSERT_NE(x, nullptr);
  clux_value_set_tdz(x, true);
  clux_value_t *out = clux_context_consume_value(ctx, x, NULL);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(out)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(out), "use of a value before initialization");

  /* TDZ 标记不被 consume 破坏：重复消费仍拒绝 */
  clux_value_t *again = clux_context_consume_value(ctx, x, NULL);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(again)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(again), "use of a value before initialization");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: consume_value 对非 TDZ 值（整数/bool/error/shadow）原值放行
   预期: 返回指针与入参同一（零拷贝透传） */
TEST(EngineTest, ConsumeValuePassesNonTdzThroughByIdentity) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *i32v = clux_context_create_i32(ctx, 42, NULL, NULL);
  ASSERT_NE(i32v, nullptr);
  EXPECT_EQ(clux_context_consume_value(ctx, i32v, NULL), i32v);

  clux_value_t *bv = clux_context_create_bool(ctx, true, NULL, NULL);
  ASSERT_NE(bv, nullptr);
  EXPECT_EQ(clux_context_consume_value(ctx, bv, NULL), bv);

  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "probe");
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(clux_context_consume_value(ctx, err, NULL), err);

  clux_value_t *shadow = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  ASSERT_NE(shadow, nullptr);
  EXPECT_EQ(clux_context_consume_value(ctx, shadow, NULL), shadow);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: TDZ 的 const 目标首次赋值（TDZ 旁路 mutable 检查）
   预期: "=" 写入成功、TDZ 清除、值正确；清除后 const 恢复不可写
         （"cannot assign to a value of type 'const i32'"，回归保护） */
TEST(EngineTest, AssignTdzConstTargetFirstAssignmentBypassesConst) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *const_i32 = clux_type_get_qualified_type(
      clux_context_builtin_type(ctx, CLUX_TYPE_I32), CLUX_TYPE_CONST);
  ptr_handle_t data =
      safe_alloc(clux_context_get_safe_allocator(ctx), sizeof(int32_t), NULL);
  ASSERT_NE(data, (ptr_handle_t)0);
  clux_value_t *target =
      clux_context_create_value(ctx, const_i32, data, "ct", NULL);
  ASSERT_NE(target, nullptr);
  clux_value_set_tdz(target, true);

  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  ASSERT_NE(five, nullptr);
  clux_value_t *res = clux_context_assign(ctx, target, five, NULL);
  EXPECT_EQ(res, target) << "first assignment must return the target itself";
  EXPECT_EQ(read_i32(target), 5);
  EXPECT_FALSE(clux_value_is_tdz(target))
      << "successful assignment must clear the TDZ flag";
  EXPECT_EQ(clux_context_consume_value(ctx, target, NULL), target)
      << "after first assignment the value becomes consumable";

  clux_value_t *bad = clux_context_assign(ctx, target, five, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "cannot assign to a value of type 'const i32'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: TDZ 的非 const（裸 i32）目标赋值
   预期: 写入成功、TDZ 清除，随后 consume 放行 */
TEST(EngineTest, AssignTdzNonConstTargetClearsFlag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *target = clux_context_create_i32(ctx, 0, "nt", NULL);
  ASSERT_NE(target, nullptr);
  clux_value_set_tdz(target, true);
  clux_value_t *forty_two = clux_context_create_i32(ctx, 42, NULL, NULL);
  ASSERT_NE(forty_two, nullptr);

  clux_value_t *res = clux_context_assign(ctx, target, forty_two, NULL);
  EXPECT_EQ(res, target);
  EXPECT_EQ(read_i32(target), 42);
  EXPECT_FALSE(clux_value_is_tdz(target));
  EXPECT_EQ(clux_context_consume_value(ctx, target, NULL), target);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- create_tdz_value：TDZ 变量构造 ---- */

/* 场景: create_tdz_value 基本属性（i32 类型）
   预期: 值创建成功且 is_tdz=true、类型正确、data 非 0（真实存储）、
         存储清零（确定性）、scope 可按名 lookup 到同一指针 */
TEST(EngineTest, CreateTdzValueBasics) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_value_t *value =
      clux_context_create_tdz_value(ctx, i32_type, "tdz_new", NULL);
  ASSERT_NE(value, nullptr);
  EXPECT_TRUE(clux_value_is_tdz(value)) << "TDZ creation must set the flag";
  EXPECT_EQ(clux_value_get_type(value), i32_type);
  EXPECT_NE(clux_value_get_data(value), (ptr_handle_t)0)
      << "TDZ value must own real (non-shadow) storage";
  EXPECT_EQ(read_i32(value), 0) << "TDZ storage is zero-initialized";
  EXPECT_EQ(clux_context_lookup(ctx, "tdz_new", NULL), value)
      << "TDZ value must be declared into the current scope";

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: type 为 NULL
   预期: 返回 error 值，消息精确为 "TDZ value creation requires a type" */
TEST(EngineTest, CreateTdzValueRequiresType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *value =
      clux_context_create_tdz_value(ctx, NULL, "tdz_notype", NULL);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "TDZ value creation requires a type");
  /* 错误值不标 TDZ */
  EXPECT_FALSE(clux_value_is_tdz(value));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 同名冲突（先创建具名 i32 值，再以同名 create_tdz_value）
   预期: 第二次返回 "duplicate declaration '<name>'" 错误值且不标 TDZ；
         原值不受影响（仍非 TDZ、可正常消费） */
TEST(EngineTest, CreateTdzValueDuplicateNameErrorsUnmarked) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *original = clux_context_create_i32(ctx, 7, "tdz_dup", NULL);
  ASSERT_NE(original, nullptr);
  clux_value_t *conflict = clux_context_create_tdz_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), "tdz_dup", NULL);
  ASSERT_NE(conflict, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(conflict)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(conflict), "duplicate declaration 'tdz_dup'");
  EXPECT_FALSE(clux_value_is_tdz(conflict))
      << "error value returned on conflict must not be TDZ-marked";

  /* 原值未被标记、未被覆盖 */
  EXPECT_FALSE(clux_value_is_tdz(original));
  EXPECT_EQ(read_i32(original), 7);
  EXPECT_EQ(clux_context_consume_value(ctx, original, NULL), original);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: TDZ 生命周期闭环（create → consume 拒绝 → assign 首赋值 → 消费放行）
   预期: consume 报 "use of a value before initialization"；首次 assign 写入
         成功、TDZ 清除；随后 consume 放行且读到写入值 */
TEST(EngineTest, CreateTdzValueLifecycleConsumeAssignRead) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *value = clux_context_create_tdz_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), "tdz_cycle", NULL);
  ASSERT_NE(value, nullptr);
  EXPECT_TRUE(clux_value_is_tdz(value));

  clux_value_t *early = clux_context_consume_value(ctx, value, NULL);
  ASSERT_NE(early, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(early)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(early), "use of a value before initialization");

  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  ASSERT_NE(five, nullptr);
  clux_value_t *assigned = clux_context_assign(ctx, value, five, NULL);
  EXPECT_EQ(assigned, value)
      << "first assignment must return the target itself";
  EXPECT_EQ(read_i32(value), 5);
  EXPECT_FALSE(clux_value_is_tdz(value))
      << "successful first assignment must clear the TDZ flag";
  EXPECT_EQ(clux_context_consume_value(ctx, value, NULL), value)
      << "after first assignment the value becomes consumable";

  delete_clux_context(&ctx);
  delete_allocator(&a);
}
