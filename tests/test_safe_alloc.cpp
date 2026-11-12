#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/location.h"
#include "core/safe_alloc.h"
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

TEST(SafeAllocTest, BasicAccessAndBounds) {
  allocator_t *a = create_allocator(malloc, free);
  safe_allocator_t *sa = create_safe_allocator(a);
  ptr_handle_t handle = safe_alloc(sa, 16, NULL);
  ASSERT_NE(handle, 0u);
  EXPECT_TRUE(ptr_is_valid(handle));
  EXPECT_EQ(ptr_get_size(handle), 16u);

  void *raw = ptr_to_raw(handle, 0);
  ASSERT_NE(raw, nullptr);
  memset(raw, 0xAB, 16);
  EXPECT_EQ(ptr_to_raw(handle, 15), (char *)raw + 15);
  EXPECT_EQ(ptr_to_raw(handle, 16), nullptr);
  EXPECT_EQ(ptr_to_raw(handle, 100), nullptr);

  EXPECT_TRUE(safe_free(handle));
  EXPECT_FALSE(ptr_is_valid(handle));
  EXPECT_EQ(ptr_get_size(handle), 0u);
  EXPECT_EQ(ptr_to_raw(handle, 0), nullptr);
  EXPECT_FALSE(safe_free(handle));

  delete_safe_allocator(&sa);
  delete_allocator(&a);
}

TEST(SafeAllocTest, OffsetViewsAndInvalidation) {
  allocator_t *a = create_allocator(malloc, free);
  safe_allocator_t *sa = create_safe_allocator(a);
  ptr_handle_t root = safe_alloc(sa, 16, NULL);
  ASSERT_NE(root, 0u);
  char *base = (char *)ptr_to_raw(root, 0);
  ASSERT_NE(base, nullptr);

  ptr_handle_t view = ptr_offset(root, 4);
  ASSERT_NE(view, 0u);
  EXPECT_EQ(ptr_get_size(view), 12u);
  EXPECT_EQ(ptr_to_raw(view, 0), base + 4);
  EXPECT_EQ(ptr_to_raw(view, 11), base + 15);
  EXPECT_EQ(ptr_to_raw(view, 12), nullptr);

  ptr_handle_t nested = ptr_offset(view, 4);
  ASSERT_NE(nested, 0u);
  EXPECT_EQ(ptr_get_size(nested), 8u);
  EXPECT_EQ(ptr_to_raw(nested, 0), base + 8);

  EXPECT_EQ(ptr_offset(root, 17), 0u);
  ptr_handle_t empty = ptr_offset(root, 16);
  ASSERT_NE(empty, 0u);
  EXPECT_EQ(ptr_get_size(empty), 0u);
  EXPECT_EQ(ptr_to_raw(empty, 0), nullptr);

  EXPECT_TRUE(safe_free(root));
  EXPECT_FALSE(ptr_is_valid(view));
  EXPECT_FALSE(ptr_is_valid(nested));
  EXPECT_FALSE(ptr_is_valid(empty));
  EXPECT_EQ(ptr_to_raw(view, 0), nullptr);
  EXPECT_FALSE(safe_free(view));

  delete_safe_allocator(&sa);
  delete_allocator(&a);
}

TEST(SafeAllocTest, ActiveListAndLocation) {
  allocator_t *a = create_allocator(malloc, free);
  safe_allocator_t *sa = create_safe_allocator(a);
  location_t loc;
  memset(&loc, 0, sizeof(loc));
  loc.filename = "test.cx";
  loc.begin.line = 3;
  loc.begin.column = 7;

  ptr_handle_t first = safe_alloc(sa, 8, &loc);
  ptr_handle_t second = safe_alloc(sa, 16, NULL);
  ASSERT_NE(first, 0u);
  ASSERT_NE(second, 0u);
  EXPECT_EQ(safe_allocator_active_count(sa), 2u);

  safe_alloc_info_t info;
  ASSERT_TRUE(safe_allocator_get_active(sa, 0, &info));
  EXPECT_EQ(info.handle, first);
  EXPECT_EQ(info.size, 8u);
  EXPECT_TRUE(info.has_location);
  EXPECT_STREQ(info.location.filename, "test.cx");
  EXPECT_EQ(info.location.begin.line, 3u);
  EXPECT_EQ(info.location.begin.column, 7u);

  ASSERT_TRUE(safe_allocator_get_active(sa, 1, &info));
  EXPECT_EQ(info.handle, second);
  EXPECT_EQ(info.size, 16u);
  EXPECT_FALSE(info.has_location);
  EXPECT_FALSE(safe_allocator_get_active(sa, 2, &info));

  EXPECT_TRUE(safe_free(first));
  EXPECT_EQ(safe_allocator_active_count(sa), 1u);
  ASSERT_TRUE(safe_allocator_get_active(sa, 0, &info));
  EXPECT_EQ(info.handle, second);

  delete_safe_allocator(&sa);
  delete_allocator(&a);
}

TEST(SafeAllocTest, DeleteInvalidatesAllHandles) {
  allocator_t *a = create_allocator(malloc, free);
  safe_allocator_t *sa = create_safe_allocator(a);
  ptr_handle_t handle = safe_alloc(sa, 8, NULL);
  ASSERT_NE(handle, 0u);
  delete_safe_allocator(&sa);
  EXPECT_FALSE(ptr_is_valid(handle));
  EXPECT_EQ(ptr_get_size(handle), 0u);
  EXPECT_FALSE(safe_free(handle));
  delete_allocator(&a);
}

TEST(SafeAllocTest, MultipleAllocatorsAreIsolated) {
  allocator_t *a = create_allocator(malloc, free);
  safe_allocator_t *first = create_safe_allocator(a);
  safe_allocator_t *second = create_safe_allocator(a);
  ptr_handle_t first_handle = safe_alloc(first, 8, NULL);
  ptr_handle_t second_handle = safe_alloc(second, 8, NULL);
  ASSERT_NE(first_handle, 0u);
  ASSERT_NE(second_handle, 0u);
  EXPECT_EQ(safe_allocator_active_count(first), 1u);
  EXPECT_EQ(safe_allocator_active_count(second), 1u);

  delete_safe_allocator(&first);
  EXPECT_FALSE(ptr_is_valid(first_handle));
  EXPECT_TRUE(ptr_is_valid(second_handle));
  EXPECT_EQ(safe_allocator_active_count(second), 1u);

  delete_safe_allocator(&second);
  EXPECT_FALSE(ptr_is_valid(second_handle));
  delete_allocator(&a);
}

TEST(SafeAllocTest, NoLeak) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  safe_allocator_t *sa = create_safe_allocator(a);
  ptr_handle_t first = safe_alloc(sa, 8, NULL);
  ptr_handle_t second = safe_alloc(sa, 16, NULL);
  ASSERT_NE(first, 0u);
  ASSERT_NE(second, 0u);
  EXPECT_TRUE(safe_free(first));
  delete_safe_allocator(&sa);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
