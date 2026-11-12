#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/string.h"
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

TEST(StringTest, CreateInitialState) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  EXPECT_EQ(string_get_size(s), 0u);
  EXPECT_EQ(string_get_capacity(s), 8u);
  ASSERT_NE(string_get_data(s), nullptr);
  EXPECT_STREQ(string_get_data(s), "");
  delete_string(&s);
  EXPECT_EQ(s, nullptr);
  delete_allocator(&a);
}

TEST(StringTest, DeleteNullIsNoop) {
  string_t *s = nullptr;
  delete_string(&s);
  delete_string(nullptr);
}

TEST(StringTest, AppendBytes) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  EXPECT_EQ(string_append(s, "abc", 3), 3u);
  EXPECT_EQ(string_get_size(s), 3u);
  EXPECT_STREQ(string_get_data(s), "abc");
  EXPECT_EQ(string_append(s, "de", 2), 5u);
  EXPECT_STREQ(string_get_data(s), "abcde");
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, AppendCstrAndChar) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  EXPECT_EQ(string_append_cstr(s, "ab"), 2u);
  EXPECT_EQ(string_append_char(s, 'c'), 3u);
  EXPECT_STREQ(string_get_data(s), "abc");
  delete_string(&s);
  delete_allocator(&a);
}

/* 逐字节追加必须按几何增长扩容，避免每次 append 都精确重分配导致 O(n^2)。 */
TEST(StringTest, AppendGrowsGeometrically) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  for (size_t i = 0; i < 10000; i++) {
    ASSERT_EQ(string_append_char(s, 'x'), i + 1);
  }
  EXPECT_EQ(string_get_size(s), 10000u);
  EXPECT_GT(string_get_capacity(s), string_get_size(s) + 1);
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, AppendNull) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  EXPECT_EQ(string_append(s, nullptr, 0), 0u);
  EXPECT_EQ(string_append(s, nullptr, 1), (size_t)-1);
  EXPECT_EQ(string_append_cstr(s, nullptr), (size_t)-1);
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, AppendSelfAlias) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "abc");
  EXPECT_EQ(string_append(s, string_get_data(s), string_get_size(s)), 6u);
  EXPECT_STREQ(string_get_data(s), "abcabc");
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, AssignReplacesContent) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "hello");
  EXPECT_EQ(string_assign(s, "hi", 2), 2u);
  EXPECT_EQ(string_get_size(s), 2u);
  EXPECT_STREQ(string_get_data(s), "hi");
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, AssignSelfSlice) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "abcdef");
  EXPECT_EQ(string_assign(s, string_get_data(s) + 2, 3), 3u);
  EXPECT_STREQ(string_get_data(s), "cde");
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, ReserveGrowsAndPreserves) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "abc");
  string_t *r = string_reserve(s, 32);
  EXPECT_EQ(r, s);
  EXPECT_GE(string_get_capacity(s), 32u);
  EXPECT_STREQ(string_get_data(s), "abc");
  size_t cap = string_get_capacity(s);
  string_reserve(s, 4);
  EXPECT_EQ(string_get_capacity(s), cap);
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, ResizeGrowsWithZeros) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "ab");
  string_resize(s, 5);
  EXPECT_EQ(string_get_size(s), 5u);
  EXPECT_EQ(string_get(s, 0), 'a');
  EXPECT_EQ(string_get(s, 1), 'b');
  EXPECT_EQ(string_get(s, 2), '\0');
  EXPECT_EQ(string_get(s, 4), '\0');
  EXPECT_EQ(string_get_data(s)[5], '\0');
  string_resize(s, 1);
  EXPECT_EQ(string_get_size(s), 1u);
  EXPECT_STREQ(string_get_data(s), "a");
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, GetAndSet) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "abc");
  EXPECT_EQ(string_get(s, 1), 'b');
  EXPECT_EQ(string_get(s, 3), '\0');
  EXPECT_TRUE(string_set(s, 1, 'x'));
  EXPECT_STREQ(string_get_data(s), "axc");
  EXPECT_FALSE(string_set(s, 3, 'x'));
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, InsertAtHeadMiddleTail) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "bd");
  EXPECT_EQ(string_insert(s, 0, "a", 1), 3u);
  EXPECT_STREQ(string_get_data(s), "abd");
  EXPECT_EQ(string_insert(s, 2, "c", 1), 4u);
  EXPECT_STREQ(string_get_data(s), "abcd");
  EXPECT_EQ(string_insert(s, 4, "e", 1), 5u);
  EXPECT_STREQ(string_get_data(s), "abcde");
  EXPECT_EQ(string_insert(s, 6, "x", 1), (size_t)-1);
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, InsertSelfAlias) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "abc");
  EXPECT_EQ(string_insert(s, 1, string_get_data(s), 3), 6u);
  EXPECT_STREQ(string_get_data(s), "aabcbc");
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, InsertSelfSliceOverlappingShift) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "abc");
  EXPECT_EQ(string_insert(s, 0, string_get_data(s) + 1, 2), 5u);
  EXPECT_STREQ(string_get_data(s), "bcabc");
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, EraseRanges) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "abcde");
  EXPECT_EQ(string_erase(s, 0, 1), 4u);
  EXPECT_STREQ(string_get_data(s), "bcde");
  EXPECT_EQ(string_erase(s, 1, 2), 2u);
  EXPECT_STREQ(string_get_data(s), "be");
  EXPECT_EQ(string_erase(s, 1, 1), 1u);
  EXPECT_STREQ(string_get_data(s), "b");
  EXPECT_EQ(string_erase(s, 2, 0), (size_t)-1);
  EXPECT_EQ(string_erase(s, 0, 2), (size_t)-1);
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, PushAndPop) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  EXPECT_EQ(string_push(s, 'a'), 1u);
  EXPECT_EQ(string_push(s, 'b'), 2u);
  EXPECT_STREQ(string_get_data(s), "ab");
  EXPECT_EQ(string_pop(s), 1u);
  EXPECT_STREQ(string_get_data(s), "a");
  EXPECT_EQ(string_pop(s), 0u);
  EXPECT_EQ(string_pop(s), (size_t)-1);
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, Clear) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  string_append_cstr(s, "abc");
  string_clear(s);
  EXPECT_EQ(string_get_size(s), 0u);
  EXPECT_STREQ(string_get_data(s), "");
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, EmbeddedNulBytes) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  EXPECT_EQ(string_append(s, "a\0b", 3), 3u);
  EXPECT_EQ(string_get_size(s), 3u);
  EXPECT_EQ(string_get(s, 0), 'a');
  EXPECT_EQ(string_get(s, 1), '\0');
  EXPECT_EQ(string_get(s, 2), 'b');
  EXPECT_EQ(string_get_data(s)[3], '\0');
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, LargeGrowth) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *s = create_string(a);
  for (int i = 0; i < 1000; i++) {
    EXPECT_EQ(string_append_char(s, 'x'), (size_t)(i + 1));
  }
  EXPECT_EQ(string_get_size(s), 1000u);
  EXPECT_GE(string_get_capacity(s), 1001u);
  EXPECT_EQ(strlen(string_get_data(s)), 1000u);
  delete_string(&s);
  delete_allocator(&a);
}

TEST(StringTest, NoLeak) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  string_t *s = create_string(a);
  string_append_cstr(s, "hello");
  string_append_char(s, ' ');
  string_append_cstr(s, "world");
  string_insert(s, 0, ">> ", 3);
  string_erase(s, 0, 3);
  string_assign(s, "abc", 3);
  string_resize(s, 64);
  string_clear(s);
  string_push(s, 'x');
  string_pop(s);
  delete_string(&s);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
