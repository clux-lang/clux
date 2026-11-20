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
#include "engine/scope.h"
#include "engine/slice.h"
#include "engine/type.h"
#include "engine/tuple.h"
#include "engine/template.h"
#include "engine/type_value.h"
#include "engine/union.h"
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
  clux_type_seal(custom); /* 布局定局即完整，方可作为值类型 */
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
  clux_type_seal(custom); /* 布局定局即完整，方可作为值类型 */
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
  /* 显式 as 通道：指针 ↔ 指针一律放行（*void 恢复 / 不相关指针互转） */
  EXPECT_NE(vt->cast, nullptr);

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

/* ---- *void：类型擦除边界（C 风格吸收，白皮书 §2.1）----
   opaque 类型已移除：擦除载荷由 *void 承载。语义规则：
   - 任意 *T → *void 隐式放行（吸收，handle 不变）；
   - *void → *T 必须显式 as（指针 ↔ 指针一律放行）；
   - *void 不可解引用——base void 永不 seal，deref 创建 void 值被
     完整性门禁拒绝（"cannot create a value of incomplete type"）。 */

TEST(EngineTest, VoidPtrLayoutAndNameLookup) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *void_ptr = clux_context_get_ptr_type(ctx, void_type);
  ASSERT_NE(void_ptr, nullptr);
  EXPECT_STREQ(clux_type_get_name(void_ptr), "*void");
  EXPECT_EQ(clux_type_get_size(void_ptr), sizeof(ptr_handle_t));
  EXPECT_EQ(clux_type_get_align(void_ptr), alignof(ptr_handle_t));
  /* 指针即时定局：构造完成即 seal（定宽不依赖 base 完整） */
  EXPECT_TRUE(clux_type_is_seal(void_ptr));

  /* *void 是派生类型而非内建：同一 base 去重，跨查询返回同一实例 */
  EXPECT_EQ(clux_context_get_ptr_type(ctx, void_type), void_ptr);

  /* "opaque" 名字已随类型一起移除：按名查找落入未声明错误 */
  clux_value_t *gone = clux_context_lookup(ctx, "opaque", NULL);
  ASSERT_NE(gone, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(gone)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(gone), "use of undeclared identifier 'opaque'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, VoidPtrImplicitFromAnyPointer) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *void_ptr = clux_context_get_ptr_type(ctx, void_type);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  clux_value_t *x = clux_context_create_i32(ctx, 10, NULL, NULL);
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  ASSERT_NE(p, nullptr);
  clux_value_t *slot = clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  ASSERT_NE(slot, nullptr);

  /* 任何指针隐式吸收进 *void：assign 经 ptr 的 safe_cast 放行 */
  clux_value_t *assigned = clux_context_assign(ctx, slot, p, NULL);
  EXPECT_EQ(assigned, slot);
  EXPECT_EQ(clux_value_get_type(slot), void_ptr);

  /* 显式转回具体指针后 handle 未丢：可解引用读回原值 */
  clux_value_t *back = clux_context_cast(ctx, slot, ptr_i32, NULL);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(clux_value_get_type(back), ptr_i32);
  clux_value_t *read = clux_context_deref(ctx, back, NULL);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read_i32(read), 10);

  /* *void 不可解引用：base void 永不 seal，deref 落入完整性门禁 */
  clux_value_t *bad_deref = clux_context_deref(ctx, slot, NULL);
  ASSERT_NE(bad_deref, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_deref)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_deref),
               "cannot create a value of incomplete type 'void'");

  /* const 限定指针同样可隐式吸收 */
  clux_type_t *const_i32 =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST);
  clux_type_t *ptr_const = clux_context_get_ptr_type(ctx, const_i32);
  clux_value_t *pcx = clux_context_create_ptr(
      ctx, ptr_const, clux_value_get_data(x), NULL, NULL);
  clux_value_t *slot2 = clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, slot2, pcx, NULL), slot2);

  /* *void 也可显式转成限定指针（unwrap 分派） */
  clux_value_t *back_const = clux_context_cast(ctx, slot2, ptr_const, NULL);
  ASSERT_NE(back_const, nullptr);
  EXPECT_EQ(clux_value_get_type(back_const), ptr_const);
  clux_value_t *read_const = clux_context_deref(ctx, back_const, NULL);
  ASSERT_NE(read_const, nullptr);
  EXPECT_EQ(read_i32(read_const), 10);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, VoidPtrCastRules) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *u8_type = clux_context_builtin_type(ctx, CLUX_TYPE_U8);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *void_ptr = clux_context_get_ptr_type(ctx, void_type);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);
  clux_type_t *ptr_u8 = clux_context_get_ptr_type(ctx, u8_type);

  clux_value_t *x = clux_context_create_i32(ctx, 10, NULL, NULL);
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  clux_value_t *slot = clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  ASSERT_EQ(clux_context_assign(ctx, slot, p, NULL), slot);

  /* 显式 as 只放行指针族：转基础类型拒绝 */
  clux_value_t *bad = clux_context_cast(ctx, slot, i32_type, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "cannot cast '*void' to 'i32'");

  /* 隐式转出拒绝：*void → *i32 赋值必须显式 as */
  clux_value_t *ptr_slot = clux_context_create_ptr(ctx, ptr_i32, 0, NULL, NULL);
  clux_value_t *bad2 = clux_context_assign(ctx, ptr_slot, slot, NULL);
  ASSERT_NE(bad2, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad2)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad2), "cannot safely cast '*void' to '*i32'");

  /* 转自身是恒等 */
  EXPECT_EQ(clux_context_cast(ctx, slot, void_ptr, NULL), slot);

  /* 不相关指针显式互转放行：*i32 as *u8（显式 as 是担责通道） */
  clux_value_t *cross = clux_context_cast(ctx, p, ptr_u8, NULL);
  ASSERT_NE(cross, nullptr);
  EXPECT_EQ(clux_value_get_type(cross), ptr_u8);

  /* 非指针源走到 *void 也不放行：*void 只从指针族隐式吸收 */
  clux_value_t *n = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *bad3 = clux_context_assign(ctx, slot, n, NULL);
  ASSERT_NE(bad3, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad3)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad3), "cannot safely cast 'i32' to '*void'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, VoidPtrEqNe) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *void_ptr = clux_context_get_ptr_type(ctx, void_type);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *y = clux_context_create_i32(ctx, 2, NULL, NULL);
  clux_value_t *ox = clux_context_create_ptr(
      ctx, void_ptr, clux_value_get_data(x), NULL, NULL);
  clux_value_t *ox2 = clux_context_create_ptr(
      ctx, void_ptr, clux_value_get_data(x), NULL, NULL);
  clux_value_t *oy = clux_context_create_ptr(
      ctx, void_ptr, clux_value_get_data(y), NULL, NULL);
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

  /* 与具体指针可比较：同为指针族，按 handle 判等（语义对齐 ptr 家族） */
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  clux_value_t *cross_eq = clux_context_binary(ctx, CLUX_OP_EQ, ox, p, NULL);
  ASSERT_NE(cross_eq, nullptr);
  EXPECT_EQ(read_bool(cross_eq), true);

  /* i32 值不是指针族：比较拒绝 */
  clux_value_t *bad = clux_context_binary(ctx, CLUX_OP_EQ, ox, y, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "operator '==' requires operands of the same type");

  /* shadow 比较产出 shadow bool */
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, void_ptr);
  clux_value_t *shadow_eq =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow, ox, NULL);
  ASSERT_NE(shadow_eq, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_eq));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, VoidPtrShadowPassthrough) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *void_ptr = clux_context_get_ptr_type(ctx, void_type);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  /* shadow 指针隐式擦除：assign 静默返回目标 */
  clux_value_t *shadow_ptr = clux_context_create_shadow_value(ctx, ptr_i32);
  clux_value_t *slot = clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  EXPECT_EQ(clux_context_assign(ctx, slot, shadow_ptr, NULL), slot);

  /* shadow *void 显式转出：产出目标类型的 shadow */
  clux_value_t *shadow_void_ptr =
      clux_context_create_shadow_value(ctx, void_ptr);
  clux_value_t *out = clux_context_cast(ctx, shadow_void_ptr, ptr_i32, NULL);
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(out));
  EXPECT_EQ(clux_value_get_type(out), ptr_i32);

  /* shadow *void 隐式转出依旧拒绝（shadow 不改变转换合法性） */
  clux_value_t *ptr_slot = clux_context_create_ptr(ctx, ptr_i32, 0, NULL, NULL);
  clux_value_t *bad = clux_context_assign(ctx, ptr_slot, shadow_void_ptr, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "cannot safely cast '*void' to '*i32'");

  /* shadow *void 解引用同样落入完整性门禁：编译期静态拒绝 */
  clux_value_t *bad_deref = clux_context_deref(ctx, shadow_void_ptr, NULL);
  ASSERT_NE(bad_deref, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_deref)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_deref),
               "cannot create a value of incomplete type 'void'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: ?*void 边界语义——optional 是独立类型而非指针包装：转换通道
   （safe_cast/cast）不穿透 optional 壁，?*i32 → ?*void 无隐式通道，
   ?*void 整体 as 也无通道；#some 边界内做 safe_cast 吸收（v as-if
   safe_cast T 后提升为 ?T），恢复则发生在离开边界之后（先 #take 再 as）
   预期: 直连通道报精确错误；边界外链路 #none/#is_none/#some/#take/as 全通 */
TEST(EngineTest, VoidPtrOptionalWrapChannels) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *void_ptr = clux_context_get_ptr_type(ctx, void_type);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);
  clux_type_t *opt_void_ptr = clux_context_get_optional_type(ctx, void_ptr);
  ASSERT_NE(opt_void_ptr, nullptr);
  EXPECT_STREQ(clux_type_get_name(opt_void_ptr), "?*void");

  clux_value_t *x = clux_context_create_i32(ctx, 30, NULL, NULL);
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  ASSERT_NE(p, nullptr);

  /* #some 边界内做 safe_cast 吸收: *i32 直接提升为 ?*void */
  clux_value_t *some =
      clux_context_create_optional_some(ctx, void_ptr, p, NULL, NULL);
  ASSERT_NE(some, nullptr);
  EXPECT_EQ(clux_value_get_type(some), opt_void_ptr);

  /* 反向不可吸收: *void 载荷进 #some(*i32) 被 safe_cast 拒绝 */
  clux_value_t *erased = clux_context_safe_cast(ctx, p, void_ptr, NULL);
  ASSERT_NE(erased, nullptr);
  EXPECT_EQ(clux_value_get_type(erased), void_ptr);
  clux_value_t *bad_some =
      clux_context_create_optional_some(ctx, ptr_i32, erased, NULL, NULL);
  ASSERT_NE(bad_some, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_some)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_some),
               "cannot safely cast '*void' to '*i32'");

  /* 边界规则: ?*i32 → ?*void 无隐式通道——转换不穿透 optional 壁 */
  clux_value_t *some_pi32 =
      clux_context_create_optional_some(ctx, ptr_i32, p, NULL, NULL);
  ASSERT_NE(some_pi32, nullptr);
  clux_value_t *opt_slot =
      clux_context_create_optional_none(ctx, void_ptr, NULL, NULL);
  ASSERT_NE(opt_slot, nullptr);
  clux_value_t *bad_assign =
      clux_context_assign(ctx, opt_slot, some_pi32, NULL);
  ASSERT_NE(bad_assign, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_assign)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_assign),
               "type '?*i32' cannot be safely cast to '?*void'");

  /* #none 字面 + #is_none 判空 */
  clux_value_t *none_q =
      clux_context_create_optional_none(ctx, void_ptr, NULL, NULL);
  ASSERT_NE(none_q, nullptr);
  EXPECT_EQ(clux_value_get_type(none_q), opt_void_ptr);
  EXPECT_TRUE(read_bool(clux_context_is_none(ctx, none_q, NULL)));
  EXPECT_FALSE(read_bool(clux_context_is_none(ctx, some, NULL)));

  /* #take 解包回 *void，显式 as 恢复具体指针，handle 未丢 */
  clux_value_t *taken = clux_context_take(ctx, some, NULL);
  ASSERT_NE(taken, nullptr);
  EXPECT_EQ(clux_value_get_type(taken), void_ptr);
  clux_value_t *restored = clux_context_cast(ctx, taken, ptr_i32, NULL);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(clux_value_get_type(restored), ptr_i32);
  EXPECT_EQ(read_i32(clux_context_deref(ctx, restored, NULL)), 30);

  /* 边界规则: ?*void 整体 as *i32 无通道——解包（#take）后方可行 as */
  clux_value_t *bad_cast = clux_context_cast(ctx, some, ptr_i32, NULL);
  ASSERT_NE(bad_cast, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_cast)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_cast),
               "type '?*void' cannot be cast to '*i32'");

  /* none 的 #take 是运行期错误，消息携带 ?*void 类型名 */
  clux_value_t *bad_take = clux_context_take(ctx, none_q, NULL);
  ASSERT_NE(bad_take, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_take)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_take),
               "cannot take a value from a none optional of type '?*void'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: *const void（指向 const void 的指针）——限定 base 经 kind_unwrap
   仍是 void，吸收/转出通道放行；限定 void 与 void 同命运永不 seal，
   deref(*const void) 落回完整性门禁
   预期: *i32 → *const void 隐式吸收成功；显式转出解引用读回原值；
         deref(*const void) 产出 error 且消息含 'void' 不完整 */
TEST(EngineTest, VoidPtrConstVoidBaseSemantics) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *void_ptr = clux_context_get_ptr_type(ctx, void_type);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);
  clux_type_t *const_void =
      clux_type_get_qualified_type(void_type, CLUX_TYPE_CONST);
  clux_type_t *ptr_const_void = clux_context_get_ptr_type(ctx, const_void);
  ASSERT_NE(ptr_const_void, nullptr);
  EXPECT_STREQ(clux_type_get_name(ptr_const_void), "*const void");

  clux_value_t *x = clux_context_create_i32(ctx, 10, NULL, NULL);
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  ASSERT_NE(p, nullptr);

  /* 限定 base 不影响吸收: *i32 → *const void 隐式放行 */
  clux_value_t *slot =
      clux_context_create_ptr(ctx, ptr_const_void, 0, NULL, NULL);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, slot, p, NULL), slot);
  EXPECT_EQ(clux_value_get_type(slot), ptr_const_void);

  /* *void 值同样吸收进 *const void */
  clux_value_t *erased = clux_context_create_ptr(
      ctx, void_ptr, clux_value_get_data(x), NULL, NULL);
  clux_value_t *slot2 =
      clux_context_create_ptr(ctx, ptr_const_void, 0, NULL, NULL);
  ASSERT_NE(erased, nullptr);
  ASSERT_NE(slot2, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, slot2, erased, NULL), slot2);

  /* 显式转出解引用读回原值 */
  clux_value_t *back = clux_context_cast(ctx, slot, ptr_i32, NULL);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(clux_value_get_type(back), ptr_i32);
  EXPECT_EQ(read_i32(clux_context_deref(ctx, back, NULL)), 10);

  /* deref(*const void) 落入完整性门禁：限定 void 永不 seal，
     创建 const void 值被拒绝，产出 error 且消息含 'void' */
  EXPECT_FALSE(clux_type_is_seal(const_void));
  clux_value_t *const_view = clux_context_deref(ctx, slot, NULL);
  ASSERT_NE(const_view, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(const_view)),
            CLUX_TYPE_ERROR);

  /* const 修饰指针本体（const *void）: const 目标不可再赋值 */
  clux_type_t *const_void_ptr =
      clux_type_get_qualified_type(void_ptr, CLUX_TYPE_CONST);
  clux_value_t *frozen =
      clux_context_create_ptr(ctx, const_void_ptr, 0, NULL, NULL);
  ASSERT_NE(frozen, nullptr);
  clux_value_t *frozen_assign = clux_context_assign(ctx, frozen, p, NULL);
  ASSERT_NE(frozen_assign, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(frozen_assign)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(frozen_assign),
               "cannot assign to a value of type 'const *void'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 复合 base（slice/array/optional）的指针 → *void 吸收——
   擦除通道只判目标，任意指针 base 一律放行且 handle 不变
   预期: 三类复合 base 全部吸收成功，转出后解引用/len/take 读回原值 */
TEST(EngineTest, VoidPtrCompositeBaseAbsorption) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_ptr = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_VOID));
  clux_value_t *x = clux_context_create_i32(ctx, 7, NULL, NULL);

  /* slice base: *[]i32 → *void，转出后 len 读回 */
  clux_type_t *slice_i32 = clux_context_get_slice_type(ctx, i32_type);
  clux_value_t *s = clux_context_create_slice(
      ctx, slice_i32, clux_value_get_data(x), 4, NULL, NULL);
  ASSERT_NE(s, nullptr);
  clux_type_t *ptr_slice = clux_context_get_ptr_type(ctx, slice_i32);
  clux_value_t *ps = clux_context_create_ptr(
      ctx, ptr_slice, clux_value_get_data(s), NULL, NULL);
  ASSERT_NE(ps, nullptr);
  clux_value_t *slot1 = clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  ASSERT_NE(slot1, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, slot1, ps, NULL), slot1);
  EXPECT_EQ(clux_value_get_type(slot1), void_ptr);
  clux_value_t *back_s = clux_context_cast(ctx, slot1, ptr_slice, NULL);
  ASSERT_NE(back_s, nullptr);
  clux_value_t *view_s = clux_context_deref(ctx, back_s, NULL);
  ASSERT_NE(view_s, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, view_s, NULL)), 4u);

  /* array base: *[2]i32 → *void，转出后类型往返恒等 */
  clux_type_t *arr_type = clux_context_get_array_type(ctx, i32_type, 2);
  clux_value_t *arr = clux_context_create_array(ctx, arr_type, NULL, NULL);
  ASSERT_NE(arr, nullptr);
  clux_type_t *ptr_arr = clux_context_get_ptr_type(ctx, arr_type);
  clux_value_t *pa = clux_context_create_ptr(
      ctx, ptr_arr, clux_value_get_data(arr), NULL, NULL);
  ASSERT_NE(pa, nullptr);
  clux_value_t *slot2 = clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  ASSERT_NE(slot2, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, slot2, pa, NULL), slot2);
  clux_value_t *back_a = clux_context_cast(ctx, slot2, ptr_arr, NULL);
  ASSERT_NE(back_a, nullptr);
  EXPECT_EQ(clux_value_get_type(back_a), ptr_arr);
  clux_value_t *view_a = clux_context_deref(ctx, back_a, NULL);
  ASSERT_NE(view_a, nullptr);
  EXPECT_EQ(clux_value_get_type(view_a), arr_type);

  /* optional base: *?i32 → *void，转出后 deref + #take 读回载荷 */
  clux_type_t *opt_i32 = clux_context_get_optional_type(ctx, i32_type);
  clux_value_t *v5 = clux_context_create_i32(ctx, 5, NULL, NULL);
  clux_value_t *ov =
      clux_context_create_optional_some(ctx, i32_type, v5, NULL, NULL);
  ASSERT_NE(ov, nullptr);
  clux_type_t *ptr_opt = clux_context_get_ptr_type(ctx, opt_i32);
  clux_value_t *po = clux_context_create_ptr(
      ctx, ptr_opt, clux_value_get_data(ov), NULL, NULL);
  ASSERT_NE(po, nullptr);
  clux_value_t *slot3 = clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  ASSERT_NE(slot3, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, slot3, po, NULL), slot3);
  clux_value_t *back_o = clux_context_cast(ctx, slot3, ptr_opt, NULL);
  ASSERT_NE(back_o, nullptr);
  clux_value_t *view_o = clux_context_deref(ctx, back_o, NULL);
  ASSERT_NE(view_o, nullptr);
  EXPECT_EQ(read_i32(clux_context_take(ctx, view_o, NULL)), 5);

  /* 双重指针 base: **i32 → *void，指针的指针同样吸收 */
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);
  clux_type_t *ptr_ptr_i32 = clux_context_get_ptr_type(ctx, ptr_i32);
  clux_value_t *pp = clux_context_create_ptr(
      ctx, ptr_ptr_i32, clux_value_get_data(ps), NULL, NULL);
  ASSERT_NE(pp, nullptr);
  clux_value_t *slot4 = clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  ASSERT_NE(slot4, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, slot4, pp, NULL), slot4);
  clux_value_t *back_pp = clux_context_cast(ctx, slot4, ptr_ptr_i32, NULL);
  ASSERT_NE(back_pp, nullptr);
  EXPECT_EQ(clux_value_get_type(back_pp), ptr_ptr_i32);
  clux_value_t *back_inner = clux_context_cast(ctx, back_pp, ptr_i32, NULL);
  ASSERT_NE(back_inner, nullptr);
  EXPECT_EQ(clux_value_get_type(back_inner), ptr_i32);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 结构体指针协变与 *void 擦除叠加——*Derived 既可协变 upcast 进
   *Base（回归保护），也可吸收进 *void；从 *void 恢复时可直接 as *Base
   预期: 协变隐式通道不受改动影响；*void 恢复的 Base/Derived 视图
         读到同一实例的字段 */
TEST(EngineTest, VoidPtrStructCovarianceInterplay) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_ptr = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_VOID));

  /* Base{a,b} ← Derived{a,b,c} 字段前缀兼容 */
  clux_struct_field_t base_fields[] = {
      {"a", i32_type},
      {"b", i32_type},
  };
  clux_struct_decl_t base_decl = {base_fields, 2, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *base_type =
      clux_context_create_struct_type(ctx, "Base", &base_decl);
  ASSERT_NE(base_type, nullptr);
  clux_struct_field_t derived_fields[] = {
      {"a", i32_type},
      {"b", i32_type},
      {"c", i32_type},
  };
  clux_struct_decl_t derived_decl = {
      derived_fields, 3, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *derived_type =
      clux_context_create_struct_type(ctx, "Derived", &derived_decl);
  ASSERT_NE(derived_type, nullptr);

  clux_type_t *base_ptr_type = clux_context_get_ptr_type(ctx, base_type);
  clux_type_t *derived_ptr_type = clux_context_get_ptr_type(ctx, derived_type);

  clux_value_t *d = clux_context_create_struct(ctx, derived_type, "d", NULL);
  ASSERT_NE(d, nullptr);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  clux_value_t *nine = clux_context_create_i32(ctx, 9, NULL, NULL);
  ASSERT_NE(five, nullptr);
  ASSERT_NE(nine, nullptr);
  clux_value_t *field_a = clux_context_field(ctx, d, "a", NULL);
  clux_value_t *field_c = clux_context_field(ctx, d, "c", NULL);
  ASSERT_NE(field_a, nullptr);
  ASSERT_NE(field_c, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, field_a, five, NULL), field_a);
  EXPECT_EQ(clux_context_assign(ctx, field_c, nine, NULL), field_c);

  clux_value_t *d_ptr = clux_context_ref(ctx, d, NULL);
  ASSERT_NE(d_ptr, nullptr);
  EXPECT_EQ(clux_value_get_type(d_ptr), derived_ptr_type);

  /* 协变回归保护: *Derived → *Base 隐式 upcast 仍工作 */
  clux_value_t *base_slot =
      clux_context_create_ptr(ctx, base_ptr_type, 0, NULL, NULL);
  ASSERT_NE(base_slot, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, base_slot, d_ptr, NULL), base_slot);

  /* 擦除叠加: *Derived → *void 吸收 */
  clux_value_t *void_slot =
      clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  ASSERT_NE(void_slot, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, void_slot, d_ptr, NULL), void_slot);
  EXPECT_EQ(clux_value_get_type(void_slot), void_ptr);

  /* 从 *void 直接 as *Base: Base 视图读到同一实例 */
  clux_value_t *as_base =
      clux_context_cast(ctx, void_slot, base_ptr_type, NULL);
  ASSERT_NE(as_base, nullptr);
  EXPECT_EQ(clux_value_get_type(as_base), base_ptr_type);
  EXPECT_EQ(read_i32(clux_context_field(
                ctx, clux_context_deref(ctx, as_base, NULL), "a", NULL)),
            5);

  /* 从 *void 直接 as *Derived: 派生字段不丢 */
  clux_value_t *as_derived =
      clux_context_cast(ctx, void_slot, derived_ptr_type, NULL);
  ASSERT_NE(as_derived, nullptr);
  EXPECT_EQ(clux_value_get_type(as_derived), derived_ptr_type);
  EXPECT_EQ(read_i32(clux_context_field(
                ctx, clux_context_deref(ctx, as_derived, NULL), "c", NULL)),
            9);

  /* *Base 值（协变产物）吸收进 *void 同样放行 */
  clux_value_t *void_slot2 =
      clux_context_create_ptr(ctx, void_ptr, 0, NULL, NULL);
  ASSERT_NE(void_slot2, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, void_slot2, base_slot, NULL), void_slot2);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: *void 相关 cast 通道边界——error 源自传播、恒等转换返回原值、
   零 handle 指针解引用拒绝
   预期: safe_cast/cast 对 error 源原样返回不进入分支逻辑；源目标同型
         直接返回原值；handle 无效的 deref 报带类型名的错误 */
TEST(EngineTest, VoidPtrCastBoundaryPaths) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_ptr = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_VOID));
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  /* error 源在转换入口原样传播（safe_cast 与 cast 两个通道） */
  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "boom");
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(clux_context_safe_cast(ctx, err, void_ptr, NULL), err);
  EXPECT_EQ(clux_context_cast(ctx, err, ptr_i32, NULL), err);

  clux_value_t *x = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *p =
      clux_context_create_ptr(ctx, ptr_i32, clux_value_get_data(x), NULL, NULL);
  ASSERT_NE(p, nullptr);

  /* 同型转换是恒等：直接返回原值（不走位重建） */
  EXPECT_EQ(clux_context_safe_cast(ctx, p, ptr_i32, NULL), p);
  EXPECT_EQ(clux_context_cast(ctx, p, ptr_i32, NULL), p);

  /* 零 handle 指针 deref 拒绝，消息携带指针类型名 */
  clux_value_t *null_ptr = clux_context_create_ptr(ctx, ptr_i32, 0, NULL, NULL);
  ASSERT_NE(null_ptr, nullptr);
  clux_value_t *bad_deref = clux_context_deref(ctx, null_ptr, NULL);
  ASSERT_NE(bad_deref, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_deref)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_deref),
               "cannot dereference a value of type '*i32'");

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

/* ---- clux_context_slice / array_slice / slice_slice / qual_slice ---- */

/* 构造 [len]i32 数组，元素预填非零互异值 10+i（经 engine index/assign），
   供切片测试复用。失败返回 NULL 由调用方 ASSERT。 */
static clux_value_t *make_filled_i32_array(clux_context_t *ctx, size_t len) {
  clux_type_t *arr_type = clux_context_get_array_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), len);
  clux_value_t *arr = clux_context_create_array(ctx, arr_type, NULL, NULL);
  if (!arr) {
    return nullptr;
  }
  for (size_t i = 0; i < len; i++) {
    clux_value_t *view = clux_context_index(
        ctx, arr, clux_context_create_usize(ctx, i, NULL, NULL), NULL);
    if (!view ||
        clux_type_get_kind(clux_value_get_type(view)) == CLUX_TYPE_ERROR) {
      return nullptr;
    }
    clux_value_t *init =
        clux_context_create_i32(ctx, (int32_t)(10 + i), NULL, NULL);
    if (!init || clux_context_assign(ctx, view, init, NULL) != view) {
      return nullptr;
    }
  }
  return arr;
}

/* 场景: [4]i32 数组 a[1:3] → []i32 切片值，slice_len 读长度，
         经 clux_context_index 对切片下标读取验证 ptr 偏移正确
   预期: 类型为 []i32，len 为 2，s[0]==11、s[1]==12（对应数组 elem 1/2） */
TEST(EngineTest, ArraySliceRangeAndIndex) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *lo = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *hi = clux_context_create_i32(ctx, 3, NULL, NULL);
  clux_value_t *s = clux_context_slice(ctx, arr, lo, hi, NULL);
  ASSERT_NE(s, nullptr);
  clux_type_t *slice_i32 = clux_context_get_slice_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(clux_value_get_type(s), slice_i32);

  clux_value_t *len = clux_context_slice_len(ctx, s, NULL);
  ASSERT_NE(len, nullptr);
  EXPECT_EQ(read_usize(len), 2u);

  /* a[1:3][0] 读出数组 elem 1 的值（11），证明切片 ptr 偏移正确 */
  clux_value_t *first = clux_context_index(
      ctx, s, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(read_i32(first), 11);
  clux_value_t *second = clux_context_index(
      ctx, s, clux_context_create_usize(ctx, 1, NULL, NULL), NULL);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(read_i32(second), 12);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 开放端点 a[:]、a[1:]、a[:2]（lo/hi 传 NULL）
   预期: 缺省端点取 0 / 数组长度，首末元素值验证缺省方向正确 */
TEST(EngineTest, ArraySliceOpenEndpoints) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *two = clux_context_create_i32(ctx, 2, NULL, NULL);

  /* a[:]：双开放端点 → 整段 */
  clux_value_t *whole = clux_context_slice(ctx, arr, NULL, NULL, NULL);
  ASSERT_NE(whole, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, whole, NULL)), 4u);
  EXPECT_EQ(
      read_i32(clux_context_index(
          ctx, whole, clux_context_create_usize(ctx, 0, NULL, NULL), NULL)),
      10);
  EXPECT_EQ(
      read_i32(clux_context_index(
          ctx, whole, clux_context_create_usize(ctx, 3, NULL, NULL), NULL)),
      13);

  /* a[:2]：hi 开放省略 lo 缺省 0 */
  clux_value_t *front = clux_context_slice(ctx, arr, NULL, two, NULL);
  ASSERT_NE(front, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, front, NULL)), 2u);
  EXPECT_EQ(
      read_i32(clux_context_index(
          ctx, front, clux_context_create_usize(ctx, 1, NULL, NULL), NULL)),
      11);

  /* a[1:]：lo 开放省略 hi 缺省长度 */
  clux_value_t *tail = clux_context_slice(ctx, arr, one, NULL, NULL);
  ASSERT_NE(tail, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, tail, NULL)), 3u);
  EXPECT_EQ(
      read_i32(clux_context_index(
          ctx, tail, clux_context_create_usize(ctx, 0, NULL, NULL), NULL)),
      11);
  EXPECT_EQ(
      read_i32(clux_context_index(
          ctx, tail, clux_context_create_usize(ctx, 2, NULL, NULL), NULL)),
      13);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 切片再切片（先 a[1:4] 再 s[0:2]）
   预期: 双重偏移后 s2[0]==11、s2[1]==12（数组 elem 1/2），len 为 2 */
TEST(EngineTest, SliceOfSliceDoubleOffset) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *four = clux_context_create_i32(ctx, 4, NULL, NULL);
  clux_value_t *s1 = clux_context_slice(ctx, arr, one, four, NULL);
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, s1, NULL)), 3u);

  clux_value_t *zero = clux_context_create_i32(ctx, 0, NULL, NULL);
  clux_value_t *two = clux_context_create_i32(ctx, 2, NULL, NULL);
  clux_value_t *s2 = clux_context_slice(ctx, s1, zero, two, NULL);
  ASSERT_NE(s2, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, s2, NULL)), 2u);
  EXPECT_EQ(read_i32(clux_context_index(
                ctx, s2, clux_context_create_usize(ctx, 0, NULL, NULL), NULL)),
            11);
  EXPECT_EQ(read_i32(clux_context_index(
                ctx, s2, clux_context_create_usize(ctx, 1, NULL, NULL), NULL)),
            12);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 切片写穿——对切片 s=a[1:3] 的 s[1] view 赋值
   预期: 写入经借用内存直达数组本体，数组 elem 2 可见变化 */
TEST(EngineTest, ArraySliceViewWriteThrough) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *lo = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *hi = clux_context_create_i32(ctx, 3, NULL, NULL);
  clux_value_t *s = clux_context_slice(ctx, arr, lo, hi, NULL);
  ASSERT_NE(s, nullptr);

  clux_value_t *view = clux_context_index(
      ctx, s, clux_context_create_usize(ctx, 1, NULL, NULL), NULL);
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(read_i32(view), 12);
  clux_value_t *v42 = clux_context_create_i32(ctx, 42, NULL, NULL);
  ASSERT_NE(v42, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, view, v42, NULL), view);

  /* 数组本体 elem 2 经独立通道读取可见变化 */
  clux_value_t *check = clux_context_index(
      ctx, arr, clux_context_create_usize(ctx, 2, NULL, NULL), NULL);
  ASSERT_NE(check, nullptr);
  EXPECT_EQ(read_i32(check), 42);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 负数端点（数组与切片的 lo=-1 / hi=-1）
   预期: 均报 error，消息精确为 "slice index must not be negative" */
TEST(EngineTest, SliceNegativeEndpointErrors) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *neg = clux_context_create_i32(ctx, -1, NULL, NULL);
  clux_value_t *two = clux_context_create_i32(ctx, 2, NULL, NULL);

  clux_value_t *bad_lo = clux_context_slice(ctx, arr, neg, two, NULL);
  ASSERT_NE(bad_lo, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_lo)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_lo), "slice index must not be negative");

  clux_value_t *bad_hi = clux_context_slice(ctx, arr, NULL, neg, NULL);
  ASSERT_NE(bad_hi, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_hi)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_hi), "slice index must not be negative");

  /* 切片再切片的负数端点同样拒绝 */
  clux_value_t *s = clux_context_slice(ctx, arr, two, NULL, NULL);
  ASSERT_NE(s, nullptr);
  clux_value_t *slice_bad = clux_context_slice(ctx, s, neg, NULL, NULL);
  ASSERT_NE(slice_bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(slice_bad)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(slice_bad), "slice index must not be negative");

  /* 切片再切片的负数 hi 端点（end < 0 分支） */
  clux_value_t *slice_bad_hi = clux_context_slice(ctx, s, NULL, neg, NULL);
  ASSERT_NE(slice_bad_hi, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(slice_bad_hi)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(slice_bad_hi),
               "slice index must not be negative");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 越界端点（数组 len 4 取 lo/hi=5；切片 len 3 取 lo/hi=4）
   预期: 精确消息 "slice index %lld is out of bounds for array of length 4"
         与 "slice index %lld is out of bounds for slice of length 3" */
TEST(EngineTest, SliceOutOfBoundsEndpointErrors) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);

  clux_value_t *bad_lo = clux_context_slice(ctx, arr, five, NULL, NULL);
  ASSERT_NE(bad_lo, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_lo)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_lo),
               "slice index 5 is out of bounds for array of length 4");

  clux_value_t *bad_hi = clux_context_slice(ctx, arr, NULL, five, NULL);
  ASSERT_NE(bad_hi, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_hi)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_hi),
               "slice index 5 is out of bounds for array of length 4");

  /* 切片（len 3）的越界端点消息用 "for slice of length" */
  clux_value_t *s = clux_context_slice(
      ctx, arr, one, clux_context_create_i32(ctx, 4, NULL, NULL), NULL);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, s, NULL)), 3u);
  clux_value_t *four = clux_context_create_i32(ctx, 4, NULL, NULL);
  clux_value_t *slice_bad_lo = clux_context_slice(ctx, s, four, NULL, NULL);
  ASSERT_NE(slice_bad_lo, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(slice_bad_lo)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(slice_bad_lo),
               "slice index 4 is out of bounds for slice of length 3");

  clux_value_t *slice_bad_hi = clux_context_slice(ctx, s, NULL, four, NULL);
  ASSERT_NE(slice_bad_hi, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(slice_bad_hi)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(slice_bad_hi),
               "slice index 4 is out of bounds for slice of length 3");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: lo > hi（数组 a[3:1] 与切片 s[2:1]）
   预期: 精确消息 "slice start %lld exceeds end %lld" */
TEST(EngineTest, SliceStartExceedsEndErrors) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *one = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *two = clux_context_create_i32(ctx, 2, NULL, NULL);
  clux_value_t *three = clux_context_create_i32(ctx, 3, NULL, NULL);

  clux_value_t *bad = clux_context_slice(ctx, arr, three, one, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "slice start 3 exceeds end 1");

  clux_value_t *s = clux_context_slice(ctx, arr, one, NULL, NULL);
  ASSERT_NE(s, nullptr);
  clux_value_t *slice_bad = clux_context_slice(ctx, s, two, one, NULL);
  ASSERT_NE(slice_bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(slice_bad)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(slice_bad), "slice start 2 exceeds end 1");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 非整数端点（bool true 作 lo 与 str "s" 作 hi，数组与切片各一）
   预期: index_to_i64 既有错误消息 "index must be an integer, got 'bool'" /
         "index must be an integer, got 'str'" */
TEST(EngineTest, SliceNonIntegerEndpointErrors) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *b = clux_context_create_bool(ctx, true, NULL, NULL);
  clux_value_t *str = clux_context_create_str(ctx, "s", NULL, NULL);

  clux_value_t *bad_lo = clux_context_slice(ctx, arr, b, NULL, NULL);
  ASSERT_NE(bad_lo, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_lo)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_lo),
               "index must be an integer, got 'bool'");

  clux_value_t *bad_hi = clux_context_slice(ctx, arr, NULL, str, NULL);
  ASSERT_NE(bad_hi, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_hi)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_hi), "index must be an integer, got 'str'");

  /* 切片再切片的非整数端点走同一 index_to_i64 通道 */
  clux_value_t *s = clux_context_slice(
      ctx, arr, NULL, clux_context_create_i32(ctx, 2, NULL, NULL), NULL);
  ASSERT_NE(s, nullptr);
  clux_value_t *slice_bad = clux_context_slice(ctx, s, b, NULL, NULL);
  ASSERT_NE(slice_bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(slice_bad)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(slice_bad),
               "index must be an integer, got 'bool'");

  /* 切片再切片的非整数 hi 端点（end 的 index_to_i64 错误通道） */
  clux_value_t *slice_bad_hi = clux_context_slice(ctx, s, NULL, str, NULL);
  ASSERT_NE(slice_bad_hi, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(slice_bad_hi)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(slice_bad_hi),
               "index must be an integer, got 'str'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 非数组/切片类型作聚合（i32 值）
   预期: vtable 无 slice 槽，报 "type 'i32' does not support slicing" */
TEST(EngineTest, SliceUnsupportedAggregateType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *x = clux_context_create_i32(ctx, 42, NULL, NULL);
  clux_value_t *bad = clux_context_slice(ctx, x, NULL, NULL, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "type 'i32' does not support slicing");

  /* vtable 为 NULL 的自定义类型同样落入兜底（clux_context_slice 的
     vt==NULL 分支），错误消息用类型名 */
  clux_type_t *bare = create_clux_type(
      a, CLUX_TYPE_PTR /* 任意非 error/聚合 kind */, "bare", 8, 8, NULL);
  clux_type_seal(bare); /* 布局定局即完整，方可作为值类型 */
  clux_value_t *bare_val = clux_context_create_value(ctx, bare, 0, NULL, NULL);
  ASSERT_NE(bare_val, nullptr);
  clux_value_t *bare_bad = clux_context_slice(ctx, bare_val, NULL, NULL, NULL);
  ASSERT_NE(bare_bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bare_bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bare_bad),
               "type 'bare' does not support slicing");

  delete_clux_context(&ctx);
  (void)delete_clux_type(&bare); /* 裸类型未注册进 ctx，由测试自行释放 */
  delete_allocator(&a);
}

/* 场景: NULL 聚合
   预期: 报 "slice requires an aggregate" */
TEST(EngineTest, SliceRequiresAggregate) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *bad = clux_context_slice(ctx, NULL, NULL, NULL, NULL);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad), "slice requires an aggregate");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: shadow 操作数（shadow 数组 / shadow 切片作聚合，shadow lo/hi）
   预期: 均产出切片类型的 shadow 值，不做真实越界检查 */
TEST(EngineTest, SliceShadowOperandsProduceShadowSlice) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *arr_type = clux_context_get_array_type(ctx, i32_type, 4);
  clux_type_t *slice_i32 = clux_context_get_slice_type(ctx, i32_type);

  clux_value_t *shadow_arr = clux_context_create_shadow_value(ctx, arr_type);
  clux_value_t *from_arr =
      clux_context_slice(ctx, shadow_arr, NULL, NULL, NULL);
  ASSERT_NE(from_arr, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(from_arr));
  EXPECT_EQ(clux_value_get_type(from_arr), slice_i32);

  clux_value_t *shadow_slice = clux_context_create_shadow_value(ctx, slice_i32);
  clux_value_t *from_slice =
      clux_context_slice(ctx, shadow_slice, NULL, NULL, NULL);
  ASSERT_NE(from_slice, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(from_slice));
  EXPECT_EQ(clux_value_get_type(from_slice), slice_i32);

  /* 真实数组 + shadow 端点仍产出 shadow 切片 */
  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *shadow_lo = clux_context_create_shadow_value(ctx, i32_type);
  clux_value_t *shadow_from_lo =
      clux_context_slice(ctx, arr, shadow_lo, NULL, NULL);
  ASSERT_NE(shadow_from_lo, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_from_lo));
  EXPECT_EQ(clux_value_get_type(shadow_from_lo), slice_i32);
  clux_value_t *shadow_hi = clux_context_create_shadow_value(ctx, i32_type);
  clux_value_t *shadow_from_hi =
      clux_context_slice(ctx, arr, NULL, shadow_hi, NULL);
  ASSERT_NE(shadow_from_hi, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_from_hi));
  EXPECT_EQ(clux_value_get_type(shadow_from_hi), slice_i32);

  /* 真实切片 + shadow lo / shadow hi（slice_slice 的短路边）同样产出
     shadow 切片 */
  clux_value_t *s = clux_context_slice(ctx, arr, NULL, NULL, NULL);
  ASSERT_NE(s, nullptr);
  clux_value_t *slice_shadow_lo =
      clux_context_slice(ctx, s, shadow_lo, NULL, NULL);
  ASSERT_NE(slice_shadow_lo, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(slice_shadow_lo));
  EXPECT_EQ(clux_value_get_type(slice_shadow_lo), slice_i32);
  clux_value_t *one_v = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *slice_shadow_hi =
      clux_context_slice(ctx, s, one_v, shadow_hi, NULL);
  ASSERT_NE(slice_shadow_hi, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(slice_shadow_hi));
  EXPECT_EQ(clux_value_get_type(slice_shadow_hi), slice_i32);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 空切角点 a[2:2]（len 4）与 a[4:4]，及切片上的 s[3:3]
   预期: len 0 切片可正常创建，slice_len 读出 0 */
TEST(EngineTest, SliceEmptyCornerPoints) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *two = clux_context_create_i32(ctx, 2, NULL, NULL);
  clux_value_t *four = clux_context_create_i32(ctx, 4, NULL, NULL);

  clux_value_t *mid = clux_context_slice(ctx, arr, two, two, NULL);
  ASSERT_NE(mid, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, mid, NULL)), 0u);

  clux_value_t *end = clux_context_slice(ctx, arr, four, four, NULL);
  ASSERT_NE(end, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, end, NULL)), 0u);

  /* 切片（len 3）末端空切同样可创建 */
  clux_value_t *three = clux_context_create_i32(ctx, 3, NULL, NULL);
  clux_value_t *s = clux_context_slice(ctx, arr, NULL, four, NULL);
  ASSERT_NE(s, nullptr);
  clux_value_t *empty = clux_context_slice(ctx, s, three, three, NULL);
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, empty, NULL)), 0u);

  /* start == len 的末端空切：start>0 但 start<len 为假，跳过偏移 */
  clux_value_t *empty_end = clux_context_slice(ctx, s, four, four, NULL);
  ASSERT_NE(empty_end, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, empty_end, NULL)), 0u);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: const 限定数组/切片经 qualifier 静态委托转发切片
   预期: qualified_view 解包后走 array_slice/slice_slice，值语义与
         裸类型一致（偏移读值正确） */
TEST(EngineTest, SliceQualifiedAggregateForwards) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *slice_i32 = clux_context_get_slice_type(ctx, i32_type);
  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_type_t *arr_type = clux_value_get_type(arr);

  /* const [4]i32 值（借用数组内存）→ qual_slice → array_slice */
  clux_type_t *const_arr =
      clux_type_get_qualified_type(arr_type, CLUX_TYPE_CONST);
  ASSERT_NE(const_arr, nullptr);
  clux_value_t *carr = clux_context_create_value(
      ctx, const_arr, clux_value_get_data(arr), NULL, NULL);
  ASSERT_NE(carr, nullptr);
  clux_value_t *from_const_arr =
      clux_context_slice(ctx,
                         carr,
                         clux_context_create_i32(ctx, 1, NULL, NULL),
                         clux_context_create_i32(ctx, 3, NULL, NULL),
                         NULL);
  ASSERT_NE(from_const_arr, nullptr);
  EXPECT_EQ(clux_value_get_type(from_const_arr), slice_i32);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, from_const_arr, NULL)), 2u);
  EXPECT_EQ(
      read_i32(clux_context_index(ctx,
                                  from_const_arr,
                                  clux_context_create_usize(ctx, 0, NULL, NULL),
                                  NULL)),
      11);

  /* 先取得真切片再包 const 限定 → qual_slice → slice_slice */
  clux_type_t *const_slice =
      clux_type_get_qualified_type(slice_i32, CLUX_TYPE_CONST);
  ASSERT_NE(const_slice, nullptr);
  clux_value_t *cslice = clux_context_create_value(
      ctx, const_slice, clux_value_get_data(from_const_arr), NULL, NULL);
  ASSERT_NE(cslice, nullptr);
  clux_value_t *from_const_slice =
      clux_context_slice(ctx,
                         cslice,
                         clux_context_create_i32(ctx, 1, NULL, NULL),
                         clux_context_create_i32(ctx, 2, NULL, NULL),
                         NULL);
  ASSERT_NE(from_const_slice, nullptr);
  EXPECT_EQ(clux_value_get_type(from_const_slice), slice_i32);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, from_const_slice, NULL)),
            1u);
  EXPECT_EQ(
      read_i32(clux_context_index(ctx,
                                  from_const_slice,
                                  clux_context_create_usize(ctx, 0, NULL, NULL),
                                  NULL)),
      12);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: error 操作数透传（error 作聚合 / lo / hi）
   预期: 原错误值直接返回（identity），消息保持 "probe" */
TEST(EngineTest, SliceErrorOperandPassthrough) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *arr = make_filled_i32_array(ctx, 4);
  ASSERT_NE(arr, nullptr);
  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "probe");
  ASSERT_NE(err, nullptr);

  clux_value_t *as_aggregate = clux_context_slice(ctx, err, NULL, NULL, NULL);
  EXPECT_EQ(as_aggregate, err);
  EXPECT_STREQ(error_message_of(as_aggregate), "probe");

  clux_value_t *as_lo = clux_context_slice(ctx, arr, err, NULL, NULL);
  EXPECT_EQ(as_lo, err);

  clux_value_t *as_hi = clux_context_slice(ctx, arr, NULL, err, NULL);
  EXPECT_EQ(as_hi, err);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 无效聚合切片（悬垂句柄的数组/切片、ptr=0 的切片、短内存块越出
         块尾、短块上的空切角点使 ptr_offset 偏移失败）
   预期: 数组报 "cannot slice an invalid array"，切片报
         "cannot slice an invalid slice"（data=0 是 shadow 语义会提前
         返回 shadow 切片，故无效句柄用「分配后释放」的悬垂句柄构造） */
TEST(EngineTest, SliceInvalidAggregateErrors) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *arr_type = clux_context_get_array_type(ctx, i32_type, 4);
  clux_type_t *slice_i32 = clux_context_get_slice_type(ctx, i32_type);

  /* 悬垂句柄：分配后立即释放，ptr_is_valid / slice_read 均失效 */
  ptr_handle_t dead =
      safe_alloc(clux_context_get_safe_allocator(ctx), 16, NULL);
  ASSERT_NE(dead, (ptr_handle_t)0);
  ASSERT_TRUE(safe_free(dead));

  clux_value_t *bad_arr =
      clux_context_create_value(ctx, arr_type, dead, NULL, NULL);
  ASSERT_NE(bad_arr, nullptr);
  clux_value_t *from_bad_arr =
      clux_context_slice(ctx, bad_arr, NULL, NULL, NULL);
  ASSERT_NE(from_bad_arr, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(from_bad_arr)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(from_bad_arr), "cannot slice an invalid array");

  clux_value_t *bad_slice =
      clux_context_create_value(ctx, slice_i32, dead, NULL, NULL);
  ASSERT_NE(bad_slice, nullptr);
  clux_value_t *from_bad_slice =
      clux_context_slice(ctx, bad_slice, NULL, NULL, NULL);
  ASSERT_NE(from_bad_slice, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(from_bad_slice)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(from_bad_slice),
               "cannot slice an invalid slice");

  /* ptr=0 但 len>0 的切片：块有效而目标句柄无效 */
  clux_value_t *null_target =
      clux_context_create_slice(ctx, slice_i32, 0, 4, NULL, NULL);
  ASSERT_NE(null_target, nullptr);
  clux_value_t *from_null_target =
      clux_context_slice(ctx, null_target, NULL, NULL, NULL);
  ASSERT_NE(from_null_target, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(from_null_target)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(from_null_target),
               "cannot slice an invalid slice");

  /* [4]i32 类型但底层块只有 4 字节：整段越出块尾 */
  clux_value_t *x = clux_context_create_i32(ctx, 0, NULL, NULL);
  ASSERT_NE(x, nullptr);
  clux_value_t *short_arr = clux_context_create_value(
      ctx, arr_type, clux_value_get_data(x), NULL, NULL);
  ASSERT_NE(short_arr, nullptr);
  clux_value_t *from_short_arr =
      clux_context_slice(ctx, short_arr, NULL, NULL, NULL);
  ASSERT_NE(from_short_arr, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(from_short_arr)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(from_short_arr),
               "cannot slice an invalid array");

  /* 短块上的空切角点 a[2:2]：new_len 0 跳过尺寸检查，ptr_offset 越
     出 4 字节块失败 */
  clux_value_t *two = clux_context_create_i32(ctx, 2, NULL, NULL);
  clux_value_t *bad_offset_arr =
      clux_context_slice(ctx, short_arr, two, two, NULL);
  ASSERT_NE(bad_offset_arr, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_offset_arr)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_offset_arr),
               "cannot slice an invalid array");

  /* ptr 指向 4 字节块但 len=4（需 16 字节）：尺寸不足 */
  clux_value_t *short_slice = clux_context_create_slice(
      ctx, slice_i32, clux_value_get_data(x), 4, NULL, NULL);
  ASSERT_NE(short_slice, nullptr);
  clux_value_t *from_short_slice =
      clux_context_slice(ctx, short_slice, NULL, NULL, NULL);
  ASSERT_NE(from_short_slice, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(from_short_slice)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(from_short_slice),
               "cannot slice an invalid slice");

  /* 短块切片上的空切角点 s[2:2]：ptr_offset 偏移失败 */
  clux_value_t *bad_offset_slice =
      clux_context_slice(ctx, short_slice, two, two, NULL);
  ASSERT_NE(bad_offset_slice, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_offset_slice)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_offset_slice),
               "cannot slice an invalid slice");

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

  /* #some 的载荷 safe_cast 自检：i64 → i32 窄化拒绝 */
  clux_value_t *bad_some = clux_context_create_optional_some(
      ctx, i32_type, clux_context_create_i64(ctx, 1, NULL, NULL), NULL, NULL);
  ASSERT_NE(bad_some, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_some)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_some),
               "cannot safely cast 'i64' to 'i32': narrowing conversion");

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

/* 两阶段构造（引擎层直调）：create 空壳 → add_member 逐个填充 → seal
   定布局。i32+f64：align = max(4,8) = 8，
   size = round_up(max(4,8),8) = 8（无 tag，成员共享偏移 0）。 */
TEST(EngineTest, CunionTwoPhaseBuildLayoutAndQueries) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);

  /* 空壳：size/align 未定，未 seal，成员表为空 */
  clux_type_t *t = create_clux_cunion_type(a, "Value");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(clux_type_get_kind(t), CLUX_TYPE_CUNION);
  EXPECT_STREQ(clux_type_get_name(t), "Value");
  EXPECT_EQ(clux_type_get_size(t), 0u);
  EXPECT_EQ(clux_type_get_align(t), 0u);
  EXPECT_FALSE(clux_type_is_seal(t));
  EXPECT_EQ(clux_cunion_member_count(t), 0u);
  EXPECT_EQ(clux_cunion_get_member(t, 0), nullptr);
  EXPECT_EQ(clux_cunion_find_member(t, "i"), nullptr);

  /* 逐成员填充 + seal 冻结布局 */
  EXPECT_TRUE(clux_cunion_type_add_member(t, a, "i", i32_type));
  EXPECT_TRUE(clux_cunion_type_add_member(t, a, "d", f64_type));
  clux_cunion_type_seal(t);
  EXPECT_TRUE(clux_type_is_seal(t));
  EXPECT_EQ(clux_type_get_size(t), 8u);
  EXPECT_EQ(clux_type_get_align(t), 8u);

  /* 成员表查询：声明序、名与类型一致 */
  EXPECT_EQ(clux_cunion_member_count(t), 2u);
  const clux_cunion_member_t *first = clux_cunion_get_member(t, 0);
  const clux_cunion_member_t *second = clux_cunion_get_member(t, 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_STREQ(first->name, "i");
  EXPECT_EQ(first->type, i32_type);
  EXPECT_STREQ(second->name, "d");
  EXPECT_EQ(second->type, f64_type);
  EXPECT_EQ(clux_cunion_get_member(t, 2), nullptr);
  EXPECT_EQ(clux_cunion_find_member(t, "d"), second);

  delete_clux_type(&t);
  EXPECT_EQ(t, nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* seal 冻结：seal 前 set_size/set_align 可写；seal 后 add_member 与
   set_size/set_align 全部拒绝且布局不变 */
TEST(EngineTest, CunionSealFreezesMutation) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);

  clux_type_t *t = create_clux_cunion_type(a, "Frozen");
  ASSERT_NE(t, nullptr);
  /* seal 前 size/align 通道开放（seal 会按成员重算覆盖） */
  EXPECT_TRUE(clux_type_set_size(t, 4));
  EXPECT_TRUE(clux_type_set_align(t, 4));
  EXPECT_TRUE(clux_cunion_type_add_member(t, a, "i", i32_type));
  /* i32 单成员：align 4，size = round_up(4,4) = 4 */
  clux_cunion_type_seal(t);
  EXPECT_EQ(clux_type_get_size(t), 4u);
  EXPECT_EQ(clux_type_get_align(t), 4u);

  /* seal 后：加成员拒绝，成员数不变 */
  EXPECT_FALSE(clux_cunion_type_add_member(t, a, "d", f64_type));
  EXPECT_EQ(clux_cunion_member_count(t), 1u);
  /* seal 后：set_size/set_align 拒绝且不改变布局 */
  EXPECT_FALSE(clux_type_set_size(t, 100));
  EXPECT_FALSE(clux_type_set_align(t, 32));
  EXPECT_EQ(clux_type_get_size(t), 4u);
  EXPECT_EQ(clux_type_get_align(t), 4u);
  EXPECT_TRUE(clux_type_is_seal(t));
  clux_type_seal(t); /* 幂等：重复 seal 不破坏布局 */
  EXPECT_EQ(clux_type_get_size(t), 4u);

  delete_clux_type(&t);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 重名成员拒绝 + name 自拷持有：add_member 后修改调用方 buffer，
   cunion 内部仍持有原字面名副本。 */
TEST(EngineTest, CunionAddMemberDuplicateAndNameSelfCopy) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);

  clux_type_t *t = create_clux_cunion_type(a, "Named");
  ASSERT_NE(t, nullptr);

  /* name 自拷：栈上 mutable buffer 传入后被引擎复制持有 */
  char name_buf[8];
  strcpy(name_buf, "tag");
  EXPECT_TRUE(clux_cunion_type_add_member(t, a, name_buf, i32_type));
  strcpy(name_buf, "xxx"); /* 覆写调用方 buffer */
  const clux_cunion_member_t *member = clux_cunion_find_member(t, "tag");
  ASSERT_NE(member, nullptr);
  EXPECT_STREQ(member->name, "tag"); /* 内部副本不受调用方覆写影响 */
  EXPECT_EQ(clux_cunion_find_member(t, "xxx"), nullptr);

  /* 重名成员拒绝：成员数与首个成员绑定不变 */
  EXPECT_FALSE(clux_cunion_type_add_member(t, a, "tag", f64_type));
  EXPECT_EQ(clux_cunion_member_count(t), 1u);
  EXPECT_EQ(clux_cunion_find_member(t, "tag")->type, i32_type);

  clux_cunion_type_seal(t);
  delete_clux_type(&t);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 空成员 seal 边界：无成员时 align 取下限 1；size 计算结果 0，
   由 clux_type_seal 的布局定局兜底修正为 1（空聚合 size==0 → 1、
   align==0 → 1），不 crash。 */
TEST(EngineTest, CunionEmptySealLayout) {
  allocator_t *a = create_allocator(malloc, free);
  clux_type_t *t = create_clux_cunion_type(a, "Empty");
  ASSERT_NE(t, nullptr);
  clux_cunion_type_seal(t);
  EXPECT_TRUE(clux_type_is_seal(t));
  EXPECT_EQ(clux_type_get_align(t), 1u);
  EXPECT_EQ(clux_type_get_size(t), 1u);
  EXPECT_EQ(clux_cunion_member_count(t), 0u);

  delete_clux_type(&t);
  delete_allocator(&a);
}

/* 手工两阶段与 context 一次性入口产物结构判等（成员名+类型序同即等，
   类型名不参与）；members_equal 指针数组直调的正负判定。 */
TEST(EngineTest, CunionTwoPhaseMatchesContextAndMembersEqual) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);

  clux_type_t *manual = create_clux_cunion_type(a, "Manual");
  ASSERT_NE(manual, nullptr);
  EXPECT_TRUE(clux_cunion_type_add_member(manual, a, "i", i32_type));
  EXPECT_TRUE(clux_cunion_type_add_member(manual, a, "d", f64_type));
  clux_cunion_type_seal(manual);

  clux_cunion_member_t members[] = {
      {"i", i32_type},
      {"d", f64_type},
  };
  clux_type_t *via_ctx =
      clux_context_create_cunion_type(ctx, "ViaContext", members, 2);
  ASSERT_NE(via_ctx, nullptr);
  EXPECT_TRUE(clux_type_equal(manual, via_ctx));
  EXPECT_TRUE(clux_type_equal(via_ctx, manual));
  EXPECT_EQ(clux_type_get_size(manual), clux_type_get_size(via_ctx));
  EXPECT_EQ(clux_type_get_align(manual), clux_type_get_align(via_ctx));

  /* members_equal 指针数组直调：成员名+类型按声明序一致判 true */
  const clux_cunion_member_t *same_ptrs[] = {&members[0], &members[1]};
  EXPECT_TRUE(clux_cunion_members_equal(manual, same_ptrs, 2));
  /* 成员数不同 */
  EXPECT_FALSE(clux_cunion_members_equal(manual, same_ptrs, 1));
  /* 名不同 */
  clux_cunion_member_t renamed[] = {
      {"i", i32_type},
      {"x", f64_type},
  };
  const clux_cunion_member_t *renamed_ptrs[] = {&renamed[0], &renamed[1]};
  EXPECT_FALSE(clux_cunion_members_equal(manual, renamed_ptrs, 2));
  /* 类型不同 */
  clux_cunion_member_t retyped[] = {
      {"i", i32_type},
      {"d", i32_type},
  };
  const clux_cunion_member_t *retyped_ptrs[] = {&retyped[0], &retyped[1]};
  EXPECT_FALSE(clux_cunion_members_equal(manual, retyped_ptrs, 2));
  /* 非 cunion 类型判 false */
  EXPECT_FALSE(clux_cunion_members_equal(i32_type, same_ptrs, 2));

  /* context 入口重名产出 NULL */
  clux_cunion_member_t dup[] = {
      {"i", i32_type},
      {"i", f64_type},
  };
  EXPECT_EQ(clux_context_create_cunion_type(ctx, "Dup", dup, 2), nullptr);

  delete_clux_type(&manual);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* delete_clux_type 半成品安全：未 seal 的空壳与含成员半成品均可直接
   删除（delete_meta 释放成员块与自拷 name，成员 type 借用不释放），
   配合全量 ASAN/LSan 验证无泄漏无悬垂。 */
TEST(EngineTest, CunionDeleteHalfBuiltSafe) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  /* 空 shell：仅 meta + 空 vec */
  clux_type_t *empty_shell = create_clux_cunion_type(a, "Shell");
  ASSERT_NE(empty_shell, nullptr);
  delete_clux_type(&empty_shell);
  EXPECT_EQ(empty_shell, nullptr);

  /* 含一个成员的半成品：不 seal 直接删除（name 副本与成员块须释放） */
  clux_type_t *half = create_clux_cunion_type(a, "Half");
  ASSERT_NE(half, nullptr);
  EXPECT_TRUE(clux_cunion_type_add_member(half, a, "i", i32_type));
  delete_clux_type(&half);
  EXPECT_EQ(half, nullptr);

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

/* 两阶段构造（引擎层直调）：create 空壳 → add_member 逐个填充 → seal
   定布局。i32+f64：align = max(4,8) = 8，size = round_up(8+8,8) = 16，
   payload 偏移 = align = 8。 */
TEST(EngineTest, UnionTwoPhaseBuildLayoutAndQueries) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);

  /* 空壳：size/align 未定，未 seal，成员表为空 */
  clux_type_t *t = create_clux_union_type(a, "Value");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(clux_type_get_kind(t), CLUX_TYPE_UNION);
  EXPECT_STREQ(clux_type_get_name(t), "Value");
  EXPECT_EQ(clux_type_get_size(t), 0u);
  EXPECT_EQ(clux_type_get_align(t), 0u);
  EXPECT_FALSE(clux_type_is_seal(t));
  EXPECT_EQ(clux_union_member_count(t), 0u);
  EXPECT_EQ(clux_union_get_member(t, 0), nullptr);
  EXPECT_EQ(clux_union_find_member(t, "a"), nullptr);

  /* 逐成员填充 + seal 冻结布局 */
  EXPECT_TRUE(clux_union_type_add_member(t, a, "a", i32_type));
  EXPECT_TRUE(clux_union_type_add_member(t, a, "b", f64_type));
  clux_union_type_seal(t);
  EXPECT_TRUE(clux_type_is_seal(t));
  EXPECT_EQ(clux_type_get_size(t), 16u);
  EXPECT_EQ(clux_type_get_align(t), 8u);
  EXPECT_EQ(clux_union_payload_offset(t), 8u);

  /* 成员表查询：声明序、名与类型一致 */
  EXPECT_EQ(clux_union_member_count(t), 2u);
  const clux_union_member_t *first = clux_union_get_member(t, 0);
  const clux_union_member_t *second = clux_union_get_member(t, 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_STREQ(first->name, "a");
  EXPECT_EQ(first->type, i32_type);
  EXPECT_STREQ(second->name, "b");
  EXPECT_EQ(second->type, f64_type);
  EXPECT_EQ(clux_union_get_member(t, 2), nullptr);
  EXPECT_EQ(clux_union_find_member(t, "b"), second);

  delete_clux_type(&t);
  EXPECT_EQ(t, nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* seal 冻结：seal 前 set_size/set_align 可写；seal 后 add_member 与
   set_size/set_align 全部拒绝且布局不变。 */
TEST(EngineTest, UnionSealFreezesMutation) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);

  clux_type_t *t = create_clux_union_type(a, "Frozen");
  ASSERT_NE(t, nullptr);
  /* seal 前 size/align 通道开放（seal 会按成员重算覆盖） */
  EXPECT_TRUE(clux_type_set_size(t, 4));
  EXPECT_TRUE(clux_type_set_align(t, 4));
  EXPECT_TRUE(clux_union_type_add_member(t, a, "a", i32_type));
  /* i32 单成员：align 4，size = round_up(4+4,4) = 8 */
  clux_union_type_seal(t);
  EXPECT_EQ(clux_type_get_size(t), 8u);
  EXPECT_EQ(clux_type_get_align(t), 4u);

  /* seal 后：加成员拒绝，成员数不变 */
  EXPECT_FALSE(clux_union_type_add_member(t, a, "b", f64_type));
  EXPECT_EQ(clux_union_member_count(t), 1u);
  /* seal 后：set_size/set_align 拒绝且不改变布局 */
  EXPECT_FALSE(clux_type_set_size(t, 100));
  EXPECT_FALSE(clux_type_set_align(t, 32));
  EXPECT_EQ(clux_type_get_size(t), 8u);
  EXPECT_EQ(clux_type_get_align(t), 4u);
  EXPECT_TRUE(clux_type_is_seal(t));
  clux_type_seal(t); /* 幂等：重复 seal 不破坏布局 */
  EXPECT_EQ(clux_type_get_size(t), 8u);

  delete_clux_type(&t);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 重名成员拒绝 + name 自拷持有：add_member 后修改调用方 buffer，
   union 内部仍持有原字面名副本。 */
TEST(EngineTest, UnionAddMemberDuplicateAndNameSelfCopy) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);

  clux_type_t *t = create_clux_union_type(a, "Named");
  ASSERT_NE(t, nullptr);

  /* name 自拷：栈上 mutable buffer 传入后被引擎复制持有 */
  char name_buf[8];
  strcpy(name_buf, "tag");
  EXPECT_TRUE(clux_union_type_add_member(t, a, name_buf, i32_type));
  strcpy(name_buf, "xxx"); /* 覆写调用方 buffer */
  const clux_union_member_t *member = clux_union_find_member(t, "tag");
  ASSERT_NE(member, nullptr);
  EXPECT_STREQ(member->name, "tag"); /* 内部副本不受调用方覆写影响 */
  EXPECT_EQ(clux_union_find_member(t, "xxx"), nullptr);

  /* 重名成员拒绝：成员数与首个成员绑定不变 */
  EXPECT_FALSE(clux_union_type_add_member(t, a, "tag", f64_type));
  EXPECT_EQ(clux_union_member_count(t), 1u);
  EXPECT_EQ(clux_union_find_member(t, "tag")->type, i32_type);

  clux_union_type_seal(t);
  delete_clux_type(&t);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 空成员 seal 边界：无成员时 align 取 tag u32 下限 4，
   size = round_up(4+0,4) = 4。 */
TEST(EngineTest, UnionEmptySealLayout) {
  allocator_t *a = create_allocator(malloc, free);
  clux_type_t *t = create_clux_union_type(a, "Empty");
  ASSERT_NE(t, nullptr);
  clux_union_type_seal(t);
  EXPECT_TRUE(clux_type_is_seal(t));
  EXPECT_EQ(clux_type_get_align(t), 4u);
  EXPECT_EQ(clux_type_get_size(t), 4u);
  EXPECT_EQ(clux_union_payload_offset(t), 4u);
  EXPECT_EQ(clux_union_member_count(t), 0u);

  delete_clux_type(&t);
  delete_allocator(&a);
}

/* 手工两阶段与 context 一次性入口产物结构判等（成员名+类型序同即等，
   类型名不参与）；members_equal 指针数组直调的正负判定。 */
TEST(EngineTest, UnionTwoPhaseMatchesContextAndMembersEqual) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);

  clux_type_t *manual = create_clux_union_type(a, "Manual");
  ASSERT_NE(manual, nullptr);
  EXPECT_TRUE(clux_union_type_add_member(manual, a, "a", i32_type));
  EXPECT_TRUE(clux_union_type_add_member(manual, a, "b", f64_type));
  clux_union_type_seal(manual);

  clux_union_member_t members[] = {
      {"a", i32_type},
      {"b", f64_type},
  };
  clux_type_t *via_ctx =
      clux_context_create_union_type(ctx, "ViaContext", members, 2);
  ASSERT_NE(via_ctx, nullptr);
  EXPECT_TRUE(clux_type_equal(manual, via_ctx));
  EXPECT_TRUE(clux_type_equal(via_ctx, manual));
  EXPECT_EQ(clux_type_get_size(manual), clux_type_get_size(via_ctx));
  EXPECT_EQ(clux_type_get_align(manual), clux_type_get_align(via_ctx));

  /* members_equal 指针数组直调：成员名+类型按声明序一致判 true */
  const clux_union_member_t *same_ptrs[] = {&members[0], &members[1]};
  EXPECT_TRUE(clux_union_members_equal(manual, same_ptrs, 2));
  /* 成员数不同 */
  EXPECT_FALSE(clux_union_members_equal(manual, same_ptrs, 1));
  /* 名不同 */
  clux_union_member_t renamed[] = {
      {"a", i32_type},
      {"x", f64_type},
  };
  const clux_union_member_t *renamed_ptrs[] = {&renamed[0], &renamed[1]};
  EXPECT_FALSE(clux_union_members_equal(manual, renamed_ptrs, 2));
  /* 类型不同 */
  clux_union_member_t retyped[] = {
      {"a", i32_type},
      {"b", i32_type},
  };
  const clux_union_member_t *retyped_ptrs[] = {&retyped[0], &retyped[1]};
  EXPECT_FALSE(clux_union_members_equal(manual, retyped_ptrs, 2));
  /* 非 union 类型判 false */
  EXPECT_FALSE(clux_union_members_equal(i32_type, same_ptrs, 2));

  /* context 入口重名产出 NULL（错误消息经 exec e2e 覆盖） */
  clux_union_member_t dup[] = {
      {"a", i32_type},
      {"a", f64_type},
  };
  EXPECT_EQ(clux_context_create_union_type(ctx, "Dup", dup, 2), nullptr);

  delete_clux_type(&manual);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 递归预备：seal 前成员类型可携带另一个未 seal 的壳。
   嵌套关系 A { b: B }、B { x: i32 }：布局读取发生在 seal 时刻，
   须先 seal 叶（B）再 seal 根（A）。B：align 4，size 8；
   A：align max(4,4) = 4，size = round_up(4+8,4) = 12。 */
TEST(EngineTest, UnionRecursiveShellMembers) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  clux_type_t *shell_a = create_clux_union_type(a, "A");
  clux_type_t *shell_b = create_clux_union_type(a, "B");
  ASSERT_NE(shell_a, nullptr);
  ASSERT_NE(shell_b, nullptr);

  /* 相互引用阶段：A 的成员是尚未定局的 B 壳 */
  EXPECT_TRUE(clux_union_type_add_member(shell_a, a, "b", shell_b));
  EXPECT_TRUE(clux_union_type_add_member(shell_b, a, "x", i32_type));

  /* 先 seal 叶 B，再 seal 根 A：A 布局按定局后的 B 计算 */
  clux_union_type_seal(shell_b);
  EXPECT_EQ(clux_type_get_align(shell_b), 4u);
  EXPECT_EQ(clux_type_get_size(shell_b), 8u);
  clux_union_type_seal(shell_a);
  EXPECT_EQ(clux_type_get_align(shell_a), 4u);
  EXPECT_EQ(clux_type_get_size(shell_a), 12u);
  EXPECT_EQ(clux_union_payload_offset(shell_a), 4u);
  EXPECT_EQ(clux_union_find_member(shell_a, "b")->type, shell_b);
  EXPECT_EQ(clux_union_find_member(shell_b, "x")->type, i32_type);

  delete_clux_type(&shell_a);
  delete_clux_type(&shell_b);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* delete_clux_type 半成品安全：未 seal 的空壳与含成员半成品均可直接
   删除（context 在 add_member 失败路径会 delete 半成品），配合全量
   ASAN/LSan 验证无泄漏无悬垂。 */
TEST(EngineTest, UnionDeleteHalfBuiltSafe) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  /* 空 shell：仅 meta + 空 vec */
  clux_type_t *empty_shell = create_clux_union_type(a, "Shell");
  ASSERT_NE(empty_shell, nullptr);
  delete_clux_type(&empty_shell);
  EXPECT_EQ(empty_shell, nullptr);

  /* 含一个成员的半成品：不 seal 直接删除（name 副本与成员块须释放） */
  clux_type_t *half = create_clux_union_type(a, "Half");
  ASSERT_NE(half, nullptr);
  EXPECT_TRUE(clux_union_type_add_member(half, a, "a", i32_type));
  delete_clux_type(&half);
  EXPECT_EQ(half, nullptr);

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

/* ---- tuple 复合类型（白皮书 §2.6/§5.5）---- */

/* 场景: <i32, str> 结构去重与 meta 访问
   预期: 命名 "<i32, str>" 风格、元素序列按声明序访问；同元素序列恒等
         返回；顺序/长度/元素不同均产出不同实例 */
TEST(EngineTest, TupleTypeInterningAndMeta) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
  clux_type_t *pair[] = {i32_type, str_type};

  clux_type_t *t = clux_context_get_tuple_type(ctx, pair, 2);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(clux_type_get_kind(t), CLUX_TYPE_TUPLE);
  EXPECT_STREQ(clux_type_get_name(t), "<i32, str>");
  EXPECT_EQ(clux_tuple_elem_count(t), 2u);
  EXPECT_EQ(clux_tuple_get_elem(t, 0), i32_type);
  EXPECT_EQ(clux_tuple_get_elem(t, 1), str_type);
  EXPECT_EQ(clux_tuple_get_elem(t, 2), nullptr)
      << "out-of-bounds element access must return NULL";
  EXPECT_EQ(clux_tuple_elem_count(nullptr), 0u)
      << "NULL type must be tolerated with count 0";
  EXPECT_EQ(clux_tuple_get_elem(nullptr, 0), nullptr)
      << "NULL type must be tolerated with NULL element";

  EXPECT_EQ(clux_context_get_tuple_type(ctx, pair, 2), t)
      << "same element sequence must be interned";
  clux_type_t *swapped[] = {str_type, i32_type};
  EXPECT_NE(clux_context_get_tuple_type(ctx, swapped, 2), t)
      << "declaration order is part of tuple identity";
  EXPECT_NE(clux_context_get_tuple_type(ctx, pair, 1), t)
      << "shorter sequence must be a different type";
  clux_type_t *three[] = {i32_type, str_type, i32_type};
  EXPECT_NE(clux_context_get_tuple_type(ctx, three, 3), t)
      << "longer sequence must be a different type";

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 空元组 <>（0 元素）
   预期: 合法类型；空聚合 seal 兜底 size/align 至 1、元素数 0、命名 "<>"、
         两次获取恒等 */
TEST(EngineTest, TupleEmptyType) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *empty = clux_context_get_tuple_type(ctx, NULL, 0);
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(clux_type_get_kind(empty), CLUX_TYPE_TUPLE);
  EXPECT_STREQ(clux_type_get_name(empty), "<>");
  EXPECT_EQ(clux_type_get_size(empty), 1u);
  EXPECT_EQ(clux_type_get_align(empty), 1u);
  EXPECT_EQ(clux_tuple_elem_count(empty), 0u);
  EXPECT_EQ(clux_tuple_get_elem(empty, 0), nullptr);
  EXPECT_EQ(clux_context_get_tuple_type(ctx, NULL, 0), empty)
      << "two empty tuples must be the same instance";

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: <i8, i64> 平铺布局与元素偏移；<i64, i8> 逆序对齐填充；
        嵌套元组（元素本身是元组类型）
   预期: i8 在偏移 0、i64 在偏移 8，align=8、size=16；逆序 size 取整到
         align 仍为 16；越界 idx 返回已累积的安全偏移；嵌套元素恒等 */
TEST(EngineTest, TupleLayoutOffsetsAndNesting) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i8_type = clux_context_builtin_type(ctx, CLUX_TYPE_I8);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  clux_type_t *pair[] = {i8_type, i64_type};
  clux_type_t *t = clux_context_get_tuple_type(ctx, pair, 2);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(clux_type_get_align(t), 8u);
  EXPECT_EQ(clux_type_get_size(t), 16u);
  EXPECT_EQ(clux_tuple_elem_offset(t, 0), 0u);
  EXPECT_EQ(clux_tuple_elem_offset(t, 1), 8u)
      << "i64 must be placed at its own alignment (8)";
  EXPECT_EQ(clux_tuple_elem_offset(t, 2), 16u)
      << "out-of-bounds idx returns the accumulated flat offset";

  clux_type_t *rev_pair[] = {i64_type, i8_type};
  clux_type_t *rev = clux_context_get_tuple_type(ctx, rev_pair, 2);
  ASSERT_NE(rev, nullptr);
  EXPECT_EQ(clux_type_get_align(rev), 8u);
  EXPECT_EQ(clux_type_get_size(rev), 16u)
      << "flat size 9 rounded up to align 8 is 16";
  EXPECT_EQ(clux_tuple_elem_offset(rev, 0), 0u);
  EXPECT_EQ(clux_tuple_elem_offset(rev, 1), 8u);

  clux_type_t *only_i32[] = {i32_type};
  clux_type_t *inner = clux_context_get_tuple_type(ctx, only_i32, 1);
  ASSERT_NE(inner, nullptr);
  EXPECT_STREQ(clux_type_get_name(inner), "<i32>");
  clux_type_t *nested_pair[] = {inner, i64_type};
  clux_type_t *nested = clux_context_get_tuple_type(ctx, nested_pair, 2);
  ASSERT_NE(nested, nullptr);
  EXPECT_EQ(clux_tuple_get_elem(nested, 0), inner)
      << "tuple element can itself be a tuple type";
  EXPECT_EQ(clux_tuple_get_elem(nested, 1), i64_type);
  EXPECT_EQ(clux_tuple_elem_offset(nested, 0), 0u);
  EXPECT_EQ(clux_tuple_elem_offset(nested, 1), 8u);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 结构判等（clux_type_equal / clux_tuple_signature_equal 各分支）
   预期: 跨 context 的同元素序列结构判等 true；顺序/长度不同 false；
         非 tuple 对端 false；signature_equal 对非 tuple self、count 不
         一致、elems NULL、空序列对空元组各分支行为正确 */
TEST(EngineTest, TupleTypeEqualAndSignature) {
  allocator_t *a1 = create_allocator(malloc, free);
  clux_context_t *ctx1 = create_clux_context(a1);
  allocator_t *a2 = create_allocator(malloc, free);
  clux_context_t *ctx2 = create_clux_context(a2);

  clux_type_t *i32_a = clux_context_builtin_type(ctx1, CLUX_TYPE_I32);
  clux_type_t *i64_a = clux_context_builtin_type(ctx1, CLUX_TYPE_I64);
  clux_type_t *pair_a[] = {i32_a, i64_a};
  clux_type_t *x = clux_context_get_tuple_type(ctx1, pair_a, 2);
  ASSERT_NE(x, nullptr);

  clux_type_t *i32_b = clux_context_builtin_type(ctx2, CLUX_TYPE_I32);
  clux_type_t *i64_b = clux_context_builtin_type(ctx2, CLUX_TYPE_I64);
  clux_type_t *pair_b[] = {i32_b, i64_b};
  clux_type_t *y = clux_context_get_tuple_type(ctx2, pair_b, 2);
  ASSERT_NE(y, nullptr);
  EXPECT_NE(x, y) << "independent contexts must create distinct instances";
  EXPECT_TRUE(clux_type_equal(x, y))
      << "same element sequence must be structurally equal";

  clux_type_t *rev_b[] = {i64_b, i32_b};
  clux_type_t *rev = clux_context_get_tuple_type(ctx2, rev_b, 2);
  ASSERT_NE(rev, nullptr);
  EXPECT_FALSE(clux_type_equal(x, rev))
      << "declaration order is part of tuple identity";
  clux_type_t *only_b[] = {i32_b};
  clux_type_t *single = clux_context_get_tuple_type(ctx2, only_b, 1);
  ASSERT_NE(single, nullptr);
  EXPECT_FALSE(clux_type_equal(x, single));
  EXPECT_FALSE(clux_type_equal(x, i32_b))
      << "tuple vs non-tuple must not be equal";

  EXPECT_FALSE(clux_tuple_signature_equal(i32_a, pair_a, 2))
      << "non-tuple self must be rejected";
  EXPECT_FALSE(clux_tuple_signature_equal(x, pair_a, 1))
      << "count mismatch must be rejected";
  EXPECT_TRUE(clux_tuple_signature_equal(x, pair_a, 2));
  EXPECT_FALSE(clux_tuple_signature_equal(x, NULL, 2))
      << "NULL elems with positive count must be rejected";
  clux_type_t *empty = clux_context_get_tuple_type(ctx1, NULL, 0);
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(clux_tuple_signature_equal(empty, NULL, 0))
      << "empty sequence matches the empty tuple";
  EXPECT_FALSE(clux_tuple_signature_equal(x, NULL, 0))
      << "2-element tuple does not match an empty sequence";
  clux_type_t *rev_a[] = {i64_a, i32_a};
  EXPECT_FALSE(clux_tuple_signature_equal(x, rev_a, 2))
      << "element order must be respected";

  delete_clux_context(&ctx1);
  delete_allocator(&a1);
  delete_clux_context(&ctx2);
  delete_allocator(&a2);
}

/* 场景: tuple 按位置 index（元素类型 view、内存共享写穿）与错误路径
   预期: index 0/1 返回元素类型 view，assign 写穿到元组内联块；越界
         （==count / 负数）报 out of bounds；非整数下标报错；shadow 元组
         产出元素类型 shadow；悬垂句柄报 "cannot index an invalid
         tuple"；error 下标原样透传 */
TEST(EngineTest, TupleIndexWriteThroughAndBounds) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_type_t *pair[] = {i32_type, i64_type};
  clux_type_t *t = clux_context_get_tuple_type(ctx, pair, 2);
  ASSERT_NE(t, nullptr);
  size_t size = clux_type_get_size(t);
  ASSERT_EQ(size, 16u);

  ptr_handle_t data =
      safe_alloc(clux_context_get_safe_allocator(ctx), size, NULL);
  ASSERT_NE(data, (ptr_handle_t)0);
  memset(ptr_to_raw(data, 0), 0, size);
  clux_value_t *tuple =
      clux_context_create_value(ctx, t, data, "tuple_v", NULL);
  ASSERT_NE(tuple, nullptr);

  /* index 0：i32 view 写穿（块内偏移 0） */
  clux_value_t *v0 = clux_context_index(
      ctx, tuple, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(v0, nullptr);
  EXPECT_EQ(clux_value_get_type(v0), i32_type);
  clux_value_t *five = clux_context_create_i32(ctx, 5, NULL, NULL);
  ASSERT_NE(five, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, v0, five, NULL), v0);
  EXPECT_EQ(read_i32(v0), 5);
  EXPECT_EQ(*(int32_t *)ptr_to_raw(data, 0), 5)
      << "assignment must write through to the tuple inline block";

  /* index 1：i64 view 写穿（块内偏移 8） */
  clux_value_t *v1 = clux_context_index(
      ctx, tuple, clux_context_create_usize(ctx, 1, NULL, NULL), NULL);
  ASSERT_NE(v1, nullptr);
  EXPECT_EQ(clux_value_get_type(v1), i64_type);
  clux_value_t *seven = clux_context_create_i64(ctx, 7, NULL, NULL);
  ASSERT_NE(seven, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, v1, seven, NULL), v1);
  EXPECT_EQ(read_i64(v1), 7);
  EXPECT_EQ(*(int64_t *)((char *)ptr_to_raw(data, 0) + 8), 7);

  /* 新取的 view 读出写穿后的值（内存共享） */
  clux_value_t *again = clux_context_index(
      ctx, tuple, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(read_i32(again), 5);

  /* 越界：idx == count */
  clux_value_t *oob = clux_context_index(
      ctx, tuple, clux_context_create_usize(ctx, 2, NULL, NULL), NULL);
  ASSERT_NE(oob, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(oob)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(oob),
               "index 2 is out of bounds for tuple of length 2");
  /* 负数下标 */
  clux_value_t *neg = clux_context_index(
      ctx, tuple, clux_context_create_i32(ctx, -1, NULL, NULL), NULL);
  ASSERT_NE(neg, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(neg)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(neg),
               "index -1 is out of bounds for tuple of length 2");
  /* 非整数下标 */
  clux_value_t *not_int = clux_context_index(
      ctx, tuple, clux_context_create_bool(ctx, true, NULL, NULL), NULL);
  ASSERT_NE(not_int, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(not_int)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(not_int),
               "index must be an integer, got 'bool'");
  /* error 下标透传 */
  clux_value_t *boom = clux_context_create_error_message(ctx, NULL, "boom");
  ASSERT_NE(boom, nullptr);
  EXPECT_EQ(clux_context_index(ctx, tuple, boom, NULL), boom)
      << "error index must pass through unchanged";

  /* 悬垂句柄：idx 合法但块无效 */
  ptr_handle_t dead =
      safe_alloc(clux_context_get_safe_allocator(ctx), size, NULL);
  ASSERT_NE(dead, (ptr_handle_t)0);
  ASSERT_TRUE(safe_free(dead));
  clux_value_t *bad = clux_context_create_value(ctx, t, dead, NULL, NULL);
  ASSERT_NE(bad, nullptr);
  clux_value_t *bad_view = clux_context_index(
      ctx, bad, clux_context_create_usize(ctx, 0, NULL, NULL), NULL);
  ASSERT_NE(bad_view, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_view)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_view), "cannot index an invalid tuple");

  /* shadow 元组：产出对应元素类型的 shadow */
  clux_value_t *shadow_tuple = clux_context_create_shadow_value(ctx, t);
  ASSERT_NE(shadow_tuple, nullptr);
  clux_value_t *shadow_elem = clux_context_index(
      ctx, shadow_tuple, clux_context_create_usize(ctx, 1, NULL, NULL), NULL);
  ASSERT_NE(shadow_elem, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_elem));
  EXPECT_EQ(clux_value_get_type(shadow_elem), i64_type);

  /* shadow 下标：tuple_index 的 shadow 检查位于 index_to_i64 之后，
     shadow 值 data==0 先被拦截——当前行为是 "cannot read an invalid
     index" error（slice_index 是先查 shadow 的约定，此处为已报告的
     防错顺序疑似 bug，待主代理确认后该断言应改为元素 shadow） */
  clux_value_t *shadow_index = clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_USIZE));
  ASSERT_NE(shadow_index, nullptr);
  clux_value_t *via_shadow_index =
      clux_context_index(ctx, tuple, shadow_index, NULL);
  ASSERT_NE(via_shadow_index, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(via_shadow_index)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(via_shadow_index),
               "cannot read an invalid index");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: tuple eq/ne 字节比较与错误路径
   预期: 同类型同字节 eq 为 true；一字节差异 eq/ne 反转；类型不同（另一
         元组 / 非 tuple）报 "requires operands of the same type"（含
         '!=' 操作符文本）；悬垂句柄报 "cannot compare invalid tuples"；
         shadow 产出 bool shadow；error 操作数原样透传 */
TEST(EngineTest, TupleEqNeByteComparison) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_type_t *pair[] = {i32_type, i64_type};
  clux_type_t *t = clux_context_get_tuple_type(ctx, pair, 2);
  ASSERT_NE(t, nullptr);
  size_t size = clux_type_get_size(t);
  ASSERT_EQ(size, 16u);

  ptr_handle_t data_a =
      safe_alloc(clux_context_get_safe_allocator(ctx), size, NULL);
  ptr_handle_t data_b =
      safe_alloc(clux_context_get_safe_allocator(ctx), size, NULL);
  ASSERT_NE(data_a, (ptr_handle_t)0);
  ASSERT_NE(data_b, (ptr_handle_t)0);
  memset(ptr_to_raw(data_a, 0), 0, size);
  memset(ptr_to_raw(data_b, 0), 0, size);
  clux_value_t *left = clux_context_create_value(ctx, t, data_a, "l", NULL);
  clux_value_t *right = clux_context_create_value(ctx, t, data_b, "r", NULL);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);

  /* 全零字节：eq true / ne false */
  clux_value_t *eq = clux_context_binary(ctx, CLUX_OP_EQ, left, right, NULL);
  ASSERT_NE(eq, nullptr);
  EXPECT_TRUE(read_bool(eq));
  clux_value_t *ne = clux_context_binary(ctx, CLUX_OP_NE, left, right, NULL);
  ASSERT_NE(ne, nullptr);
  EXPECT_FALSE(read_bool(ne));

  /* 一字节差异：eq false / ne true（按字节 memcmp 语义） */
  *(int32_t *)ptr_to_raw(data_b, 0) = 1;
  clux_value_t *eq_diff =
      clux_context_binary(ctx, CLUX_OP_EQ, left, right, NULL);
  ASSERT_NE(eq_diff, nullptr);
  EXPECT_FALSE(read_bool(eq_diff));
  clux_value_t *ne_diff =
      clux_context_binary(ctx, CLUX_OP_NE, left, right, NULL);
  ASSERT_NE(ne_diff, nullptr);
  EXPECT_TRUE(read_bool(ne_diff));

  /* 不同 tuple 类型（元素序不同，size 相同） */
  clux_type_t *rev_pair[] = {i64_type, i32_type};
  clux_type_t *rev = clux_context_get_tuple_type(ctx, rev_pair, 2);
  ASSERT_NE(rev, nullptr);
  clux_value_t *rev_value =
      clux_context_create_value(ctx, rev, data_a, NULL, NULL);
  ASSERT_NE(rev_value, nullptr);
  clux_value_t *eq_rev =
      clux_context_binary(ctx, CLUX_OP_EQ, left, rev_value, NULL);
  ASSERT_NE(eq_rev, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(eq_rev)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(eq_rev),
               "operator '==' requires operands of the same type");
  clux_value_t *ne_rev =
      clux_context_binary(ctx, CLUX_OP_NE, left, rev_value, NULL);
  ASSERT_NE(ne_rev, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(ne_rev)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(ne_rev),
               "operator '!=' requires operands of the same type");

  /* 右操作数非 tuple */
  clux_value_t *plain = clux_context_create_i32(ctx, 0, NULL, NULL);
  ASSERT_NE(plain, nullptr);
  clux_value_t *eq_non_tuple =
      clux_context_binary(ctx, CLUX_OP_EQ, left, plain, NULL);
  ASSERT_NE(eq_non_tuple, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(eq_non_tuple)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(eq_non_tuple),
               "operator '==' requires operands of the same type");

  /* 悬垂句柄（右操作数无效） */
  ptr_handle_t dead =
      safe_alloc(clux_context_get_safe_allocator(ctx), size, NULL);
  ASSERT_NE(dead, (ptr_handle_t)0);
  ASSERT_TRUE(safe_free(dead));
  clux_value_t *bad = clux_context_create_value(ctx, t, dead, NULL, NULL);
  ASSERT_NE(bad, nullptr);
  clux_value_t *eq_bad = clux_context_binary(ctx, CLUX_OP_EQ, left, bad, NULL);
  ASSERT_NE(eq_bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(eq_bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(eq_bad), "cannot compare invalid tuples");

  /* 悬垂左操作数：同样报 "cannot compare invalid tuples" */
  clux_value_t *eq_bad_left =
      clux_context_binary(ctx, CLUX_OP_EQ, bad, left, NULL);
  ASSERT_NE(eq_bad_left, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(eq_bad_left)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(eq_bad_left), "cannot compare invalid tuples");

  /* shadow：产出 bool shadow */
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, t);
  ASSERT_NE(shadow, nullptr);
  clux_value_t *shadow_eq =
      clux_context_binary(ctx, CLUX_OP_EQ, shadow, left, NULL);
  ASSERT_NE(shadow_eq, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_eq));
  EXPECT_EQ(clux_value_get_type(shadow_eq),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  /* shadow 在右：同样产出 bool shadow */
  clux_value_t *shadow_right =
      clux_context_binary(ctx, CLUX_OP_NE, left, shadow, NULL);
  ASSERT_NE(shadow_right, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow_right));
  EXPECT_EQ(clux_value_get_type(shadow_right),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));

  /* error 操作数透传（左/右） */
  clux_value_t *boom = clux_context_create_error_message(ctx, NULL, "boom");
  ASSERT_NE(boom, nullptr);
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_EQ, boom, left, NULL), boom);
  EXPECT_EQ(clux_context_binary(ctx, CLUX_OP_EQ, left, boom, NULL), boom);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: clux_context_get_tuple_type 入参非法
   预期: count>0 而 elems NULL / 元素 NULL 返回 NULL（错误经
         create_error_message 记入 context 错误表，无公开访问器，按
         NULL 返回值断言）；之后合法调用不受影响 */
TEST(EngineTest, GetTupleTypeInvalidInputs) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  EXPECT_EQ(clux_context_get_tuple_type(ctx, NULL, 2), nullptr)
      << "positive count with NULL elems must be rejected";
  clux_type_t *with_null[] = {i32_type, NULL};
  EXPECT_EQ(clux_context_get_tuple_type(ctx, with_null, 2), nullptr)
      << "NULL element must be rejected";

  clux_type_t *good[] = {i32_type};
  clux_type_t *t = clux_context_get_tuple_type(ctx, good, 1);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(clux_type_get_kind(t), CLUX_TYPE_TUPLE);
  EXPECT_STREQ(clux_type_get_name(t), "<i32>");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ================= 栈帧协议 engine 侧通道 ================= */

/* 场景：swap_scope 是栈帧协议的核心通道——调用边界「保存调用方作用域 →
   进入帧 → 执行 → 恢复」。验证换入返回原作用域、current 随之切换、
   global 恒定不变，换回后 current 复原，帧 scope 生命周期归调用方。 */
TEST(EngineTest, SwapScopeExchangesAndReturnsPrevious) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_scope_t *global = clux_context_get_global_scope(ctx);
  clux_scope_t *original = clux_context_get_scope(ctx);
  ASSERT_EQ(original, global) << "fresh context starts at global scope";

  clux_scope_t *frame = create_clux_scope(a, global);
  ASSERT_NE(frame, nullptr);

  clux_scope_t *prev = clux_context_swap_scope(ctx, frame);
  EXPECT_EQ(prev, original) << "swap must return the previous scope";
  EXPECT_EQ(clux_context_get_scope(ctx), frame)
      << "current must switch to the frame";
  EXPECT_EQ(clux_context_get_global_scope(ctx), global)
      << "global must stay constant across swaps";

  clux_scope_t *back = clux_context_swap_scope(ctx, prev);
  EXPECT_EQ(back, frame);
  EXPECT_EQ(clux_context_get_scope(ctx), original);

  delete_clux_scope(&frame);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 哨兵执行器：记录调用次数与收到的六参，固定返回 i32 42——
   不解析 AST，只验证 engine 侧透传契约。 */
static int g_executor_calls;
static clux_context_t *g_executor_last_ctx;
static void *g_executor_last_ast;
static clux_value_t *g_executor_last_func;
static size_t g_executor_last_argc;

static clux_value_t *test_executor(clux_context_t *ctx,
                                   void *ast,
                                   clux_value_t *func,
                                   size_t argc,
                                   clux_value_t *argv[],
                                   const location_t *loc) {
  (void)argv;
  (void)loc;
  g_executor_calls++;
  g_executor_last_ctx = ctx;
  g_executor_last_ast = ast;
  g_executor_last_func = func;
  g_executor_last_argc = argc;
  return clux_context_create_i32(ctx, 42, NULL, NULL);
}

/* 场景：执行器安装/读取往返，invoke 对校验通过的真函数回调执行器并
   六参透传（ast 指针、argc）；函数体未挂 AST 时即使已装执行器也报
   "no executable environment" 且不回调。 */
TEST(EngineTest, ExecutorInstallAndDispatch) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "fresh context has no executor";
  clux_context_set_executor(ctx, test_executor);
  EXPECT_EQ(clux_context_get_executor(ctx), test_executor)
      << "set/get must round-trip";

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *params[] = {i32_type};
  clux_type_t *func_type =
      clux_context_create_func_type(ctx, "F", NULL, 0, params, 1, i32_type);
  ASSERT_NE(func_type, nullptr);
  clux_value_t *f = clux_context_create_func(ctx, func_type, "f", NULL);
  ASSERT_NE(f, nullptr);

  /* 已装执行器但函数体未挂 AST：运行期 error，执行器不得被回调 */
  g_executor_calls = 0;
  clux_value_t *arg = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *no_env = clux_context_invoke(ctx, f, 1, &arg, NULL);
  EXPECT_NE(strstr(error_message_of(no_env),
                   "function call has no executable environment attached"),
            nullptr);
  EXPECT_EQ(g_executor_calls, 0) << "executor must not run without an AST";

  /* 挂 AST 后 invoke 回调执行器：ast 指针与 argc 透传，返回执行器结果 */
  static int dummy_body;
  ASSERT_EQ(clux_context_func_set_ast(ctx, f, &dummy_body, NULL), f);
  clux_value_t *called = clux_context_invoke(ctx, f, 1, &arg, NULL);
  ASSERT_NE(called, nullptr);
  EXPECT_EQ(g_executor_calls, 1);
  EXPECT_EQ(g_executor_last_ctx, ctx);
  EXPECT_EQ(g_executor_last_ast, &dummy_body);
  EXPECT_EQ(g_executor_last_func, f);
  EXPECT_EQ(g_executor_last_argc, 1u);
  EXPECT_EQ(read_i32(called), 42);

  /* 零实参形态：argc 透传 0，argv NULL 容忍 */
  clux_type_t *void_fn_type =
      clux_context_create_func_type(ctx, "V", NULL, 0, NULL, 0, i32_type);
  clux_value_t *v = clux_context_create_func(ctx, void_fn_type, "v", NULL);
  ASSERT_EQ(clux_context_func_set_ast(ctx, v, &dummy_body, NULL), v);
  clux_value_t *called0 = clux_context_invoke(ctx, v, 0, NULL, NULL);
  ASSERT_NE(called0, nullptr);
  EXPECT_EQ(g_executor_calls, 2);
  EXPECT_EQ(g_executor_last_argc, 0u);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景：深度护栏默认上限 1024——1024 次 enter 全部放行，第 1025 次拒绝；
   被拒的 enter 不占槽位（leave 一次后可再进）；enter/leave 配对归零。 */
TEST(EngineTest, CallDepthDefaultLimit1024) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  EXPECT_EQ(clux_context_get_call_depth(ctx), 0u);
  for (size_t i = 0; i < 1024; i++) {
    ASSERT_TRUE(clux_context_call_depth_enter(ctx))
        << "enter #" << i << " must be allowed";
    ASSERT_EQ(clux_context_get_call_depth(ctx), i + 1);
  }
  EXPECT_FALSE(clux_context_call_depth_enter(ctx))
      << "enter #1024 (the 1025th) must be rejected";
  EXPECT_EQ(clux_context_get_call_depth(ctx), 1024u)
      << "rejected enter must not take a slot";

  clux_context_call_depth_leave(ctx);
  EXPECT_EQ(clux_context_get_call_depth(ctx), 1023u);
  EXPECT_TRUE(clux_context_call_depth_enter(ctx))
      << "after one leave a new enter must succeed";

  for (size_t i = 0; i < 1024; i++)
    clux_context_call_depth_leave(ctx);
  EXPECT_EQ(clux_context_get_call_depth(ctx), 0u)
      << "paired enter/leave must return to zero";

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景：CLUX_MAX_CALL_DEPTH 环境变量覆盖默认上限（仅在
   create_clux_context 时读取）——env=2 时第 3 次 enter 拒绝，leave 后
   可再进；用例自建 ctx 并在结尾还原环境变量。 */
TEST(EngineTest, CallDepthEnvOverride) {
  const char *prev = getenv("CLUX_MAX_CALL_DEPTH");
#ifdef _WIN32
  ASSERT_EQ(_putenv_s("CLUX_MAX_CALL_DEPTH", "2"), 0);
#else
  ASSERT_EQ(setenv("CLUX_MAX_CALL_DEPTH", "2", 1), 0);
#endif

  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  EXPECT_TRUE(clux_context_call_depth_enter(ctx));
  EXPECT_TRUE(clux_context_call_depth_enter(ctx));
  EXPECT_FALSE(clux_context_call_depth_enter(ctx))
      << "env=2 must cap enters at two";
  EXPECT_EQ(clux_context_get_call_depth(ctx), 2u);

  clux_context_call_depth_leave(ctx);
  EXPECT_TRUE(clux_context_call_depth_enter(ctx))
      << "leave must free a slot under env override";

  delete_clux_context(&ctx);
  delete_allocator(&a);

  /* 结尾必还原环境变量（EXPECT 失败不跳过本段；用例无提前 return） */
  if (prev) {
#ifdef _WIN32
    EXPECT_EQ(_putenv_s("CLUX_MAX_CALL_DEPTH", prev), 0);
#else
    EXPECT_EQ(setenv("CLUX_MAX_CALL_DEPTH", prev, 1), 0);
#endif
  } else {
#ifdef _WIN32
    EXPECT_EQ(_putenv_s("CLUX_MAX_CALL_DEPTH", ""), 0);
#else
    unsetenv("CLUX_MAX_CALL_DEPTH");
#endif
  }
}

/* 场景：return signal API 语义——signal 的 type kind 是 RETURN_SIGNAL，
   is_return_signal 判别，payload 读取返回借用（指针同一）；非 signal
   值判别 false/载荷 NULL；NULL 入参安全（false/NULL）；裸 signal
   （无载荷）payload 为 NULL。 */
TEST(EngineTest, ReturnSignalApiSemantics) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  clux_value_t *payload = clux_context_create_i32(ctx, 9, NULL, NULL);
  clux_value_t *signal = clux_context_create_return_signal(ctx, payload);
  ASSERT_NE(signal, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(signal)),
            CLUX_TYPE_RETURN_SIGNAL);
  EXPECT_TRUE(clux_value_is_return_signal(signal));
  clux_value_t *read_back = clux_return_signal_get_payload(signal);
  ASSERT_NE(read_back, nullptr);
  EXPECT_EQ(read_back, payload) << "payload reading must be a borrow";
  EXPECT_EQ(read_i32(read_back), 9);

  /* 非 signal 值：判别 false，载荷 NULL */
  clux_value_t *plain = clux_context_create_i32(ctx, 1, NULL, NULL);
  EXPECT_FALSE(clux_value_is_return_signal(plain));
  EXPECT_EQ(clux_return_signal_get_payload(plain), nullptr);

  /* NULL 入参：安全拒绝 */
  EXPECT_FALSE(clux_value_is_return_signal(NULL));
  EXPECT_EQ(clux_return_signal_get_payload(NULL), nullptr);

  /* 裸 signal（return; 无值形态）：payload NULL */
  clux_value_t *bare = clux_context_create_return_signal(ctx, NULL);
  ASSERT_NE(bare, nullptr);
  EXPECT_TRUE(clux_value_is_return_signal(bare));
  EXPECT_EQ(clux_return_signal_get_payload(bare), nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, EnumTypeStrUnderlyingDirectCreate) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
  ASSERT_NE(str_type, nullptr);

  /* 合法路径：str 底层全员有字符串常量 */
  clux_enum_variant_t variants[] = {
      {"Red", 0, "red"},
      {"Green", 0, "green"},
  };
  clux_type_t *enum_type =
      clux_context_create_enum_type(ctx, "Se", str_type, variants, 2);
  ASSERT_NE(enum_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(enum_type), CLUX_TYPE_ENUM);
  EXPECT_STREQ(clux_type_get_name(enum_type), "Se");
  EXPECT_EQ(clux_enum_variant_count(enum_type), 2u);
  EXPECT_EQ(clux_enum_get_underlying(enum_type), str_type);
  EXPECT_EQ(clux_type_get_size(enum_type), sizeof(char *));
  EXPECT_EQ(clux_type_get_size(enum_type), clux_type_get_size(str_type));
  EXPECT_EQ(clux_type_get_align(enum_type), clux_type_get_align(str_type));
  const clux_enum_variant_t *red = clux_enum_get_variant(enum_type, 0);
  ASSERT_NE(red, nullptr);
  EXPECT_STREQ(red->name, "Red");
  EXPECT_STREQ(red->str, "red");
  EXPECT_EQ(red->value, 0u) << "str variants must not use the value field";
  const clux_enum_variant_t *found = clux_enum_find_variant(enum_type, "Green");
  ASSERT_NE(found, nullptr);
  EXPECT_STREQ(found->str, "green");

  /* 结构去重：同一 str 成员表（声明名不同）恒等返回同一实例 */
  clux_enum_variant_t same[] = {
      {"Red", 0, "red"},
      {"Green", 0, "green"},
  };
  EXPECT_EQ(clux_context_create_enum_type(ctx, "Other", str_type, same, 2),
            enum_type);

  /* str 底层缺字符串常量：返回 NULL 并记录 error（错误入 context 内部
     队列，无公开 accessor，仅能断言 NULL 返回） */
  clux_enum_variant_t missing_str[] = {
      {"Red", 0, NULL},
  };
  EXPECT_EQ(clux_context_create_enum_type(ctx, "Ms", str_type, missing_str, 1),
            nullptr);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- enum cast：vtable 委托 underlying 决定 cast/safe_cast 合法性 ---- */

/* 构造持有指定位型的枚举值：存储块走 context 的 safe_allocator（context
   销毁统一释放），bits 的低 size 字节即值表示（布局与 underlying 一致）。 */
static clux_value_t *make_enum_value_with_bits(clux_context_t *ctx,
                                               clux_type_t *enum_type,
                                               const char *name,
                                               uint64_t bits) {
  size_t size = clux_type_get_size(enum_type);
  ptr_handle_t data =
      safe_alloc(clux_context_get_safe_allocator(ctx), size, NULL);
  if (data == 0 || ptr_to_raw(data, 0) == NULL) {
    return NULL;
  }
  memcpy(ptr_to_raw(data, 0), &bits, size);
  return clux_context_create_value(ctx, enum_type, data, name, NULL);
}

/* 构造 i32 底层枚举类型 { Red=1, Green=2, Blue=4 } */
static clux_type_t *make_i32_color_enum(clux_context_t *ctx, const char *name) {
  clux_enum_variant_t variants[] = {
      {"Red", 1, NULL},
      {"Green", 2, NULL},
      {"Blue", 4, NULL},
  };
  return clux_context_create_enum_type(
      ctx, name, clux_context_builtin_type(ctx, CLUX_TYPE_I32), variants, 3);
}

/* 场景: 枚举 cast/safe_cast 宽化——enum Color: i32 值 2 as i64
   预期: 委托底层 i32 的 vtable 通道，宽化成功，产出 i64 值且位型正确 */
TEST(EngineTest, EnumCastWidensUnderlyingToLargerInteger) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *value = make_enum_value_with_bits(ctx, color, NULL, 2);
  ASSERT_NE(value, nullptr);

  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_value_t *safe_result =
      clux_context_safe_cast(ctx, value, i64_type, NULL);
  ASSERT_NE(safe_result, nullptr);
  EXPECT_EQ(clux_value_get_type(safe_result), i64_type);
  EXPECT_EQ(read_i64(safe_result), 2);

  clux_value_t *result = clux_context_cast(ctx, value, i64_type, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), i64_type);
  EXPECT_EQ(read_i64(result), 2);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: safe_cast narrowing 拒绝——enum Color: i32 as i8
   预期: 消息以底层类型名报告 "cannot safely cast 'i32' to 'i8':
         narrowing conversion"（underlying 决定规则，设计如此） */
TEST(EngineTest, EnumSafeCastNarrowingRejectedByUnderlying) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *value = make_enum_value_with_bits(ctx, color, NULL, 300);
  ASSERT_NE(value, nullptr);

  clux_value_t *result = clux_context_safe_cast(
      ctx, value, clux_context_builtin_type(ctx, CLUX_TYPE_I8), NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "cannot safely cast 'i32' to 'i8': narrowing conversion");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 显式 cast 允许 narrowing——enum Color: i32 值 300 as i8
   预期: 宽松通道放行，位型按 i8 截断（300 & 0xFF == 44） */
TEST(EngineTest, EnumCastExplicitNarrowingTruncates) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *value = make_enum_value_with_bits(ctx, color, NULL, 300);
  ASSERT_NE(value, nullptr);

  clux_value_t *result = clux_context_cast(
      ctx, value, clux_context_builtin_type(ctx, CLUX_TYPE_I8), NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I8));
  EXPECT_EQ(read_i8(result), 44);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 枚举 as 自身底层恒等——enum Color: i32 值 2 as i32
   预期: 恒等委托命中（视图类型==effective target），复制位型重新注册为
         i32 值；结果与原值不是同一实例但位型相同 */
TEST(EngineTest, EnumCastToUnderlyingIdentityReregisters) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *value = make_enum_value_with_bits(ctx, color, NULL, 2);
  ASSERT_NE(value, nullptr);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_value_t *safe_result =
      clux_context_safe_cast(ctx, value, i32_type, NULL);
  ASSERT_NE(safe_result, nullptr);
  EXPECT_NE(safe_result, value)
      << "identity delegation must re-register a copy";
  EXPECT_EQ(clux_value_get_type(safe_result), i32_type);
  EXPECT_EQ(read_i32(safe_result), 2);

  clux_value_t *result = clux_context_cast(ctx, value, i32_type, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_NE(result, value);
  EXPECT_EQ(clux_value_get_type(result), i32_type);
  EXPECT_EQ(read_i32(result), 2);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 枚举 as 同布局枚举——E1: i32 { A=1, B=2 } 值 7 as E2: i32
        { C=1, D=2 }（成员名不同，结构判等不同实例）
   预期: 产出 E2 类型的枚举值，位型保留（7） */
TEST(EngineTest, EnumCastToAnotherEnumKeepsTargetLayout) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_enum_variant_t e1_variants[] = {
      {"A", 1, NULL},
      {"B", 2, NULL},
  };
  clux_type_t *e1 = clux_context_create_enum_type(
      ctx, "E1", clux_context_builtin_type(ctx, CLUX_TYPE_I32), e1_variants, 2);
  ASSERT_NE(e1, nullptr);
  clux_enum_variant_t e2_variants[] = {
      {"C", 1, NULL},
      {"D", 2, NULL},
  };
  clux_type_t *e2 = clux_context_create_enum_type(
      ctx, "E2", clux_context_builtin_type(ctx, CLUX_TYPE_I32), e2_variants, 2);
  ASSERT_NE(e2, nullptr);
  ASSERT_NE(e1, e2) << "different variant names must not dedup";

  clux_value_t *value = make_enum_value_with_bits(ctx, e1, NULL, 7);
  ASSERT_NE(value, nullptr);

  clux_value_t *safe_result = clux_context_safe_cast(ctx, value, e2, NULL);
  ASSERT_NE(safe_result, nullptr);
  EXPECT_EQ(clux_value_get_type(safe_result), e2);
  EXPECT_EQ(read_i32(safe_result), 7);

  clux_value_t *result = clux_context_cast(ctx, value, e2, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), e2);
  EXPECT_EQ(read_i32(result), 7);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 枚举 as 更窄底层的枚举——E1: i32 值 300 as E2: i16
   预期: 宽松 cast 允许（i32→i16 整数显式 narrowing 合法），产出 E2 枚举
         值且布局按 i16 落地（值 300 i16 域内保留）；safe_cast 拒绝并按
         底层名报 narrowing */
TEST(EngineTest, EnumCastToEnumWithNarrowerUnderlying) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_enum_variant_t e1_variants[] = {
      {"A", 1, NULL},
  };
  clux_type_t *e1 = clux_context_create_enum_type(
      ctx, "E1", clux_context_builtin_type(ctx, CLUX_TYPE_I32), e1_variants, 1);
  ASSERT_NE(e1, nullptr);
  clux_enum_variant_t e2_variants[] = {
      {"C", 1, NULL},
  };
  clux_type_t *e2 = clux_context_create_enum_type(
      ctx, "E2", clux_context_builtin_type(ctx, CLUX_TYPE_I16), e2_variants, 1);
  ASSERT_NE(e2, nullptr);

  clux_value_t *value = make_enum_value_with_bits(ctx, e1, NULL, 300);
  ASSERT_NE(value, nullptr);

  clux_value_t *safe_result = clux_context_safe_cast(ctx, value, e2, NULL);
  ASSERT_NE(safe_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(safe_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(safe_result),
               "cannot safely cast 'i32' to 'i16': narrowing conversion");

  clux_value_t *result = clux_context_cast(ctx, value, e2, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), e2);
  EXPECT_EQ(clux_type_get_size(clux_value_get_type(result)),
            clux_type_get_size(clux_context_builtin_type(ctx, CLUX_TYPE_I16)));
  EXPECT_EQ(read_i16(result), 300);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: str 底层枚举 cast——underlying str 无 cast/safe_cast 槽位
   预期: 报错且源/目标名用枚举自身名字（非 underlying 名） */
TEST(EngineTest, EnumCastStrUnderlyingReportsEnumName) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_enum_variant_t variants[] = {
      {"Red", 0, "red"},
      {"Green", 0, "green"},
  };
  clux_type_t *se = clux_context_create_enum_type(
      ctx, "Se", clux_context_builtin_type(ctx, CLUX_TYPE_STR), variants, 2);
  ASSERT_NE(se, nullptr);

  clux_value_t *value =
      make_enum_value_with_bits(ctx, se, NULL, (uint64_t)(uintptr_t)"red");
  ASSERT_NE(value, nullptr);

  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_value_t *safe_result =
      clux_context_safe_cast(ctx, value, i32_type, NULL);
  ASSERT_NE(safe_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(safe_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(safe_result),
               "cannot safely cast 'Se' to 'i32'");

  clux_value_t *result = clux_context_cast(ctx, value, i32_type, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "cannot cast 'Se' to 'i32'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 枚举 as str——integer_cast 拒绝数值到字符串
   预期: 委托产生的错误原样透传 "cannot cast 'i32' to 'str'"
         （消息为底层类型名，underlying 决定规则） */
TEST(EngineTest, EnumCastIntegerUnderlyingToStrRejected) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *value = make_enum_value_with_bits(ctx, color, NULL, 2);
  ASSERT_NE(value, nullptr);

  clux_value_t *result = clux_context_cast(
      ctx, value, clux_context_builtin_type(ctx, CLUX_TYPE_STR), NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "cannot cast 'i32' to 'str'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: error 值透传 + source==target 恒等返回
   预期: error 值原样返回（同一指针）；同枚举类型指针恒等路径直接返回原值 */
TEST(EngineTest, EnumCastErrorPassthroughAndIdentity) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *err = clux_context_create_error_message(ctx, NULL, "boom");
  ASSERT_NE(err, nullptr);
  clux_value_t *from_error = clux_context_cast(ctx, err, color, NULL);
  EXPECT_EQ(from_error, err) << "error value must pass through unchanged";
  clux_value_t *from_error_safe = clux_context_safe_cast(ctx, err, color, NULL);
  EXPECT_EQ(from_error_safe, err);

  clux_value_t *value = make_enum_value_with_bits(ctx, color, NULL, 2);
  ASSERT_NE(value, nullptr);
  clux_value_t *identity = clux_context_cast(ctx, value, color, NULL);
  EXPECT_EQ(identity, value) << "source == target must return the value itself";
  clux_value_t *identity_safe = clux_context_safe_cast(ctx, value, color, NULL);
  EXPECT_EQ(identity_safe, value);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: shadow 枚举值 cast——类型检查通道的 shadow 值
   预期: 直接产出目标的 shadow 值（类型为目标类型，且仍为 shadow） */
TEST(EngineTest, EnumCastShadowValueYieldsTargetShadow) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *shadow = clux_context_create_shadow_value(ctx, color);
  ASSERT_NE(shadow, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(shadow));

  clux_type_t *i64_type = clux_context_builtin_type(ctx, CLUX_TYPE_I64);
  clux_value_t *safe_result =
      clux_context_safe_cast(ctx, shadow, i64_type, NULL);
  ASSERT_NE(safe_result, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(safe_result));
  EXPECT_EQ(clux_value_get_type(safe_result), i64_type);

  clux_value_t *result = clux_context_cast(ctx, shadow, i64_type, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(clux_value_is_shadow(result));
  EXPECT_EQ(clux_value_get_type(result), i64_type);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 枚举数值化 cast——enum Color: i32 值 2 as f64 / as bool / 值 0 as bool
   预期: integer_cast 数值转换通道放行，f64 == 2.0，bool(2) == true，
         bool(0) == false */
TEST(EngineTest, EnumCastToF64AndBoolConvertsNumerically) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *value = make_enum_value_with_bits(ctx, color, NULL, 2);
  ASSERT_NE(value, nullptr);
  clux_type_t *f64_type = clux_context_builtin_type(ctx, CLUX_TYPE_F64);
  clux_value_t *as_f64 = clux_context_cast(ctx, value, f64_type, NULL);
  ASSERT_NE(as_f64, nullptr);
  EXPECT_EQ(clux_value_get_type(as_f64), f64_type);
  EXPECT_DOUBLE_EQ(read_f64(as_f64), 2.0);

  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
  clux_value_t *as_bool = clux_context_cast(ctx, value, bool_type, NULL);
  ASSERT_NE(as_bool, nullptr);
  EXPECT_EQ(clux_value_get_type(as_bool), bool_type);
  EXPECT_TRUE(read_bool(as_bool));

  clux_value_t *zero = make_enum_value_with_bits(ctx, color, NULL, 0);
  ASSERT_NE(zero, nullptr);
  clux_value_t *false_bool = clux_context_cast(ctx, zero, bool_type, NULL);
  ASSERT_NE(false_bool, nullptr);
  EXPECT_FALSE(read_bool(false_bool));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 反向 cast——i32 值 as 枚举（cast 由 source 类型 vtable 分派）
   预期: 走 integer vtable 的 integer_cast，目标枚举非 numeric kind 被拒
         "cannot cast 'i32' to 'Color'"（锁定 source 分派行为） */
TEST(EngineTest, EnumCastIntegerSourceToEnumRejected) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *color = make_i32_color_enum(ctx, "Color");
  ASSERT_NE(color, nullptr);

  clux_value_t *i32_value = clux_context_create_i32(ctx, 2, NULL, NULL);
  ASSERT_NE(i32_value, nullptr);

  clux_value_t *result = clux_context_cast(ctx, i32_value, color, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "cannot cast 'i32' to 'Color'");

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- 全类型 seal 完整性机制 ---- */

/* 场景: void 天然不完整——永不 seal，值创建门禁（create_value 与
   create_shadow_value）均拒绝；i32 对照正常
   预期: void is_seal=false；两条通道都产出 error 值且消息精确为
         "cannot create a value of incomplete type 'void'" */
TEST(EngineTest, VoidNeverSealsRejectsValueCreation) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  ASSERT_NE(void_type, nullptr);
  EXPECT_FALSE(clux_type_is_seal(void_type))
      << "void must never seal (incomplete by nature)";

  clux_value_t *value =
      clux_context_create_value(ctx, void_type, 0, NULL, NULL);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "cannot create a value of incomplete type 'void'");

  clux_value_t *shadow = clux_context_create_shadow_value(ctx, void_type);
  ASSERT_NE(shadow, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(shadow)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(shadow),
               "cannot create a value of incomplete type 'void'");

  /* i32 对照：正常通道不受门禁影响 */
  clux_value_t *i32_value = clux_context_create_i32(ctx, 7, NULL, NULL);
  ASSERT_NE(i32_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(i32_value)), CLUX_TYPE_I32);
  EXPECT_TRUE(clux_type_is_seal(clux_value_get_type(i32_value)));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 内建类型全部创建即 seal——数值/str/type/模板单例与
   error 类型经各自通道断言；return_signal 类型经信号值反查
   预期: 所有内建类型 is_seal=true */
TEST(EngineTest, BuiltinTypesSealedOnCreation) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);

  /* 数值类型抽查 */
  EXPECT_TRUE(clux_type_is_seal(clux_context_builtin_type(ctx, CLUX_TYPE_I32)));
  EXPECT_TRUE(clux_type_is_seal(clux_context_builtin_type(ctx, CLUX_TYPE_F64)));
  EXPECT_TRUE(
      clux_type_is_seal(clux_context_builtin_type(ctx, CLUX_TYPE_USIZE)));
  EXPECT_TRUE(
      clux_type_is_seal(clux_context_builtin_type(ctx, CLUX_TYPE_BOOL)));
  EXPECT_TRUE(clux_type_is_seal(clux_context_builtin_type(ctx, CLUX_TYPE_STR)));
  /* PTR 不是 builtin_type 的标量分支（无 case 返回 NULL），指针类型经
     get_ptr_type 复合构造，seal 断言由 CompositeTypesSealOnConstruction 与
     PtrOverVoidSealedAndValueCreatable 覆盖。 */

  /* type / 模板 / error 单例 */
  EXPECT_TRUE(clux_type_is_seal(clux_context_get_type_type(ctx)));
  EXPECT_TRUE(clux_type_is_seal(clux_context_get_type_template_type(ctx)));
  EXPECT_TRUE(clux_type_is_seal(clux_context_get_func_template_type(ctx)));
  EXPECT_TRUE(clux_type_is_seal(clux_context_get_error_type(ctx)));

  /* return_signal 类型：builtin_type 无该 kind 的公开分支，经信号值
     反查其类型断言 seal（信号类型是引擎内部控制流载体，同样定宽） */
  clux_value_t *payload = clux_context_create_i32(ctx, 1, NULL, NULL);
  ASSERT_NE(payload, nullptr);
  clux_value_t *signal = clux_context_create_return_signal(ctx, payload);
  ASSERT_NE(signal, nullptr);
  EXPECT_TRUE(clux_value_is_return_signal(signal));
  EXPECT_TRUE(clux_type_is_seal(clux_value_get_type(signal)));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: 复合类型构造完成即 seal（find-or-create 与一次性入口全覆盖）
   预期: ptr/slice/array/optional/qualifier/tuple + enum/cunion/struct/
         func/union 一次构造后 is_seal=true */
TEST(EngineTest, CompositeTypesSealOnConstruction) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  ASSERT_NE(i32_type, nullptr);

  /* children 表复合类型 */
  clux_type_t *ptr_type = clux_context_get_ptr_type(ctx, i32_type);
  ASSERT_NE(ptr_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(ptr_type));
  clux_type_t *slice_type = clux_context_get_slice_type(ctx, i32_type);
  ASSERT_NE(slice_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(slice_type));
  clux_type_t *array_type = clux_context_get_array_type(ctx, i32_type, 3);
  ASSERT_NE(array_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(array_type));
  clux_type_t *optional_type = clux_context_get_optional_type(ctx, i32_type);
  ASSERT_NE(optional_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(optional_type));
  clux_type_t *const_type =
      clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST);
  ASSERT_NE(const_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(const_type));

  /* 元组 */
  clux_type_t *elems[] = {i32_type};
  clux_type_t *tuple_type = clux_context_get_tuple_type(ctx, elems, 1);
  ASSERT_NE(tuple_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(tuple_type));

  /* enum（整型底层） */
  clux_enum_variant_t variants[] = {{"Red", 0, NULL}, {"Green", 1, NULL}};
  clux_type_t *enum_type =
      clux_context_create_enum_type(ctx, "SealedE", i32_type, variants, 2);
  ASSERT_NE(enum_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(enum_type));

  /* cunion */
  clux_cunion_member_t cu_members[] = {{"i", i32_type}};
  clux_type_t *cunion_type =
      clux_context_create_cunion_type(ctx, "SealedCu", cu_members, 1);
  ASSERT_NE(cunion_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(cunion_type));

  /* struct */
  clux_struct_field_t fields[] = {{"a", i32_type}};
  clux_struct_decl_t decl = {fields, 1, NULL, 0, NULL, 0, NULL, 0};
  clux_type_t *struct_type =
      clux_context_create_struct_type(ctx, "SealedS", &decl);
  ASSERT_NE(struct_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(struct_type));

  /* func */
  clux_type_t *params[] = {i32_type};
  clux_type_t *func_type = clux_context_create_func_type(
      ctx, "SealedF", NULL, 0, params, 1, i32_type);
  ASSERT_NE(func_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(func_type));

  /* tag union（context 一次性入口内部 seal） */
  clux_union_member_t members[] = {{"a", i32_type}};
  clux_type_t *union_type =
      clux_context_create_union_type(ctx, "SealedU", members, 1);
  ASSERT_NE(union_type, nullptr);
  EXPECT_TRUE(clux_type_is_seal(union_type));

  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: tag union 两阶段构造的 seal 门禁——空壳造值拒绝、seal 后放行
   预期: 未 seal 空壳 create_shadow_value 返回 error（消息含类型名）；
         add_member + seal 后 shadow 与真值创建均放行 */
TEST(EngineTest, UnionSealGateOnValueCreation) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  ASSERT_NE(i32_type, nullptr);

  /* 空壳：未 seal，造值被门禁拒绝 */
  clux_type_t *shell = create_clux_union_type(a, "GateU");
  ASSERT_NE(shell, nullptr);
  EXPECT_FALSE(clux_type_is_seal(shell));
  clux_value_t *early = clux_context_create_shadow_value(ctx, shell);
  ASSERT_NE(early, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(early)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(early),
               "cannot create a value of incomplete type 'GateU'");

  /* seal 后：shadow 与真值都放行 */
  ASSERT_TRUE(clux_union_type_add_member(shell, a, "a", i32_type));
  clux_union_type_seal(shell);
  EXPECT_TRUE(clux_type_is_seal(shell));

  clux_value_t *shadow = clux_context_create_shadow_value(ctx, shell);
  ASSERT_NE(shadow, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(shadow)), CLUX_TYPE_UNION);
  EXPECT_TRUE(clux_value_is_shadow(shadow));

  clux_value_t *real = clux_context_create_union(ctx, shell, NULL, NULL);
  ASSERT_NE(real, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(real)), CLUX_TYPE_UNION);
  EXPECT_FALSE(clux_value_is_shadow(real));

  delete_clux_type(&shell);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 场景: ptr-over-void 语义观察——指针定宽，base 未 seal 也 seal，
   为递归 union（union A { f: *A }）铺路；*void 可造值
   预期: get_ptr_type(void) 返回非 NULL 且 is_seal；create_ptr 放行 */
TEST(EngineTest, PtrOverVoidSealedAndValueCreatable) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  ASSERT_NE(void_type, nullptr);
  EXPECT_FALSE(clux_type_is_seal(void_type));

  clux_type_t *ptr_type = clux_context_get_ptr_type(ctx, void_type);
  ASSERT_NE(ptr_type, nullptr)
      << "pointer to void must be constructible (fixed-width pointer)";
  EXPECT_EQ(clux_type_get_kind(ptr_type), CLUX_TYPE_PTR);
  EXPECT_TRUE(clux_type_is_seal(ptr_type))
      << "ptr seals even over an unsealed base";

  clux_value_t *value = clux_context_create_ptr(ctx, ptr_type, 0, NULL, NULL);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_PTR);

  delete_clux_context(&ctx);
  delete_allocator(&a);
}
