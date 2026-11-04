#include <gtest/gtest.h>
#include "core/vec.h"
#include <stdlib.h>
#include <stdint.h>

TEST(VecTest, CreateInitialState) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  EXPECT_EQ(vec_get_size(v), 0u);
  EXPECT_EQ(vec_get_capacity(v), 8u);
  delete_vec(&v);
  EXPECT_EQ(v, nullptr);
  delete_allocator(&a);
}

TEST(VecTest, PushGrowsSize) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  for (int i = 0; i < 20; i++) {
    size_t s = vec_push(v, (void *)(intptr_t)(i + 1));
    EXPECT_EQ(s, (size_t)(i + 1));
  }
  EXPECT_EQ(vec_get_size(v), 20u);
  EXPECT_GE(vec_get_capacity(v), 20u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, GetOutOfBoundsReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  EXPECT_EQ(vec_get(v, 1), nullptr);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, PopShrinks) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  vec_push(v, (void *)2);
  EXPECT_EQ(vec_pop(v), 1u);
  EXPECT_EQ(vec_get_size(v), 1u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, InsertAtHead) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)2);
  vec_push(v, (void *)3);
  vec_insert(v, 0, (void *)1);
  EXPECT_EQ(vec_get_size(v), 3u);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  EXPECT_EQ(vec_get(v, 1), (void *)2);
  EXPECT_EQ(vec_get(v, 2), (void *)3);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, InsertAtTail) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  size_t r = vec_insert(v, 1, (void *)2);
  EXPECT_NE(r, (size_t)-1);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  EXPECT_EQ(vec_get(v, 1), (void *)2);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, InsertInMiddle) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  vec_push(v, (void *)3);
  vec_insert(v, 1, (void *)2);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  EXPECT_EQ(vec_get(v, 1), (void *)2);
  EXPECT_EQ(vec_get(v, 2), (void *)3);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, EraseMiddleReturnsElement) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  vec_push(v, (void *)2);
  vec_push(v, (void *)3);
  EXPECT_EQ(vec_erase(v, 1), (void *)2);
  EXPECT_EQ(vec_get_size(v), 2u);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  EXPECT_EQ(vec_get(v, 1), (void *)3);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, EraseLastElementReturnsElement) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  vec_push(v, (void *)2);
  EXPECT_EQ(vec_erase(v, 1), (void *)2);
  EXPECT_EQ(vec_get_size(v), 1u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, EraseOutOfBoundsReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  EXPECT_EQ(vec_erase(v, 1), nullptr);
  EXPECT_EQ(vec_get_size(v), 1u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, SetReplacesAndReturnsOld) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  EXPECT_EQ(vec_set(v, 0, (void *)99), (void *)1);
  EXPECT_EQ(vec_get(v, 0), (void *)99);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, SetSamePointerReturnsSameValue) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  EXPECT_EQ(vec_set(v, 0, (void *)1), (void *)1);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, SetOutOfBoundsReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  EXPECT_EQ(vec_set(v, 1, (void *)2), nullptr);
  EXPECT_EQ(vec_get_size(v), 1u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, ReserveDoesNotShrinkSize) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  for (int i = 0; i < 10; i++)
    vec_push(v, (void *)(intptr_t)(i + 1));
  EXPECT_EQ(vec_get_size(v), 10u);
  vec_reserve(v, 5);
  EXPECT_EQ(vec_get_size(v), 10u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, ReserveGrowsCapacity) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_reserve(v, 100);
  EXPECT_GE(vec_get_capacity(v), 100u);
  EXPECT_EQ(vec_get_size(v), 0u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, ResizeGrowZeroFills) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_resize(v, 5);
  EXPECT_EQ(vec_get_size(v), 5u);
  for (size_t i = 0; i < 5; i++)
    EXPECT_EQ(vec_get(v, i), nullptr);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, ResizeShrinkKeepsPrefix) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  vec_push(v, (void *)2);
  vec_push(v, (void *)3);
  vec_resize(v, 1);
  EXPECT_EQ(vec_get_size(v), 1u);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, PopEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  EXPECT_EQ(vec_pop(v), (size_t)-1);
  delete_vec(&v);
  delete_allocator(&a);
}

/* 验证 vec 不持有元素所有权：delete_vec 不释放元素 */
TEST(VecTest, DeleteDoesNotFreeElements) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  int *p = (int *)allocator_alloc(a, sizeof(int));
  *p = 42;
  vec_push(v, p);
  delete_vec(&v);
  /* p 仍有效，调用方负责释放 */
  EXPECT_EQ(*p, 42);
  allocator_free(a, (void **)&p);
  delete_allocator(&a);
}

TEST(VecTest, InsertOutOfBoundsReturnsError) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a);
  vec_push(v, (void *)1);
  EXPECT_EQ(vec_insert(v, 5, (void *)2), (size_t)-1);
  EXPECT_EQ(vec_get_size(v), 1u);
  delete_vec(&v);
  delete_allocator(&a);
}
