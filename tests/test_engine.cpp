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
    EXPECT_NE(clux_type_get_vtable(c.type), nullptr);
    EXPECT_NE(clux_type_get_vtable(c.type)->add, nullptr);
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

TEST(EngineTest, AddMismatchedIntegerTypesProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_value_t *left = clux_context_create_i8(ctx, 1, NULL, NULL);
  clux_value_t *right = clux_context_create_i16(ctx, 2, NULL, NULL);
  clux_value_t *result = clux_context_add(ctx, left, right, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
  clux_error_t **stored =
      (clux_error_t **)ptr_to_raw(clux_value_get_data(result), 0);
  ASSERT_NE(stored, nullptr);
  EXPECT_NE(
      strstr(error_get_message(*stored), "requires operands of the same type"),
      nullptr);
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

TEST(EngineTest, AddUnsupportedTypeProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  location_t loc;
  memset(&loc, 0, sizeof(loc));
  loc.filename = "test.cx";
  clux_error_t *left_error = create_error(a, &loc, "boom");
  clux_error_t *right_error = create_error(a, &loc, "bang");
  clux_value_t *left = clux_context_create_error(ctx, left_error);
  clux_value_t *right = clux_context_create_error(ctx, right_error);
  clux_value_t *result = clux_context_add(ctx, left, right, &loc);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
  clux_error_t **stored =
      (clux_error_t **)ptr_to_raw(clux_value_get_data(result), 0);
  ASSERT_NE(stored, nullptr);
  EXPECT_NE(strstr(error_get_message(*stored), "does not support operator '+'"),
            nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(EngineTest, AddMismatchedTypesProducesError) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  location_t loc;
  memset(&loc, 0, sizeof(loc));
  loc.filename = "test.cx";
  clux_error_t *error = create_error(a, &loc, "boom");
  clux_value_t *left = clux_context_create_error(ctx, error);
  clux_value_t *right = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *result = clux_context_add(ctx, left, right, &loc);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result), clux_context_get_error_type(ctx));
  clux_error_t **stored =
      (clux_error_t **)ptr_to_raw(clux_value_get_data(result), 0);
  ASSERT_NE(stored, nullptr);
  EXPECT_NE(
      strstr(error_get_message(*stored), "requires operands of the same type"),
      nullptr);
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
  clux_context_push_scope(ctx);
  clux_context_create_i32(ctx, 3, "a", NULL);
  clux_context_pop_scope(ctx);
  delete_clux_context(&ctx);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
