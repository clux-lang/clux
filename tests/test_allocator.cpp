#include <gtest/gtest.h>
#include "core/allocator.h"
#include <stdlib.h>
#include <string.h>

TEST(AllocatorTest, CreateAndDelete) {
  allocator_t *a = create_allocator(malloc, free);
  EXPECT_NE(a, nullptr);
  delete_allocator(&a);
  EXPECT_EQ(a, nullptr);
}

TEST(AllocatorTest, AllocReturnsWritableMemory) {
  allocator_t *a = create_allocator(malloc, free);
  void *p = allocator_alloc(a, 128);
  EXPECT_NE(p, nullptr);
  memset(p, 0xAB, 128);
  allocator_free(a, &p);
  EXPECT_EQ(p, nullptr);
  delete_allocator(&a);
}

TEST(AllocatorTest, FreeNullIsNoop) {
  allocator_t *a = create_allocator(malloc, free);
  void *p = nullptr;
  allocator_free(a, &p);
  EXPECT_EQ(p, nullptr);
  delete_allocator(&a);
}

TEST(AllocatorTest, AllocZeroSize) {
  allocator_t *a = create_allocator(malloc, free);
  void *p = allocator_alloc(a, 0);
  // malloc(0) 实现定义：返回 NULL 或唯一指针，二者均合法，不应 abort
  if (p) allocator_free(a, &p);
  delete_allocator(&a);
}

TEST(AllocatorTest, DeleteNullIsNoop) {
  allocator_t *a = nullptr;
  delete_allocator(&a);
  EXPECT_EQ(a, nullptr);
  delete_allocator(nullptr);
}
