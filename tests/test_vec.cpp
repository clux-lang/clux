#include <gtest/gtest.h>
#include "core/vec.h"
#include <stdlib.h>
#include <stdint.h>

static void *dup_int(allocator_t *a, int v) {
  int *p = (int *)allocator_alloc(a, sizeof(int));
  *p = v;
  return p;
}

TEST(VecTest, CreateInitialState) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  EXPECT_EQ(vec_get_size(v), 0u);
  EXPECT_EQ(vec_get_capacity(v), 8u);
  delete_vec(&v);
  EXPECT_EQ(v, nullptr);
  delete_allocator(&a);
}

TEST(VecTest, PushGrowsSize) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
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
  vec_t *v = create_vec(a, false);
  vec_push(v, (void *)1);
  EXPECT_EQ(vec_get(v, 1), nullptr);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, PopShrinks) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  vec_push(v, (void *)1);
  vec_push(v, (void *)2);
  EXPECT_EQ(vec_pop(v), 1u);
  EXPECT_EQ(vec_get_size(v), 1u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, InsertAtHead) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
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
  vec_t *v = create_vec(a, false);
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
  vec_t *v = create_vec(a, false);
  vec_push(v, (void *)1);
  vec_push(v, (void *)3);
  vec_insert(v, 1, (void *)2);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  EXPECT_EQ(vec_get(v, 1), (void *)2);
  EXPECT_EQ(vec_get(v, 2), (void *)3);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, EraseMiddle) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  vec_push(v, (void *)1);
  vec_push(v, (void *)2);
  vec_push(v, (void *)3);
  vec_erase(v, 1);
  EXPECT_EQ(vec_get_size(v), 2u);
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  EXPECT_EQ(vec_get(v, 1), (void *)3);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, EraseLastElement) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  vec_push(v, (void *)1);
  vec_push(v, (void *)2);
  vec_erase(v, 1);
  EXPECT_EQ(vec_get_size(v), 1u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, SetReplaces) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  vec_push(v, (void *)1);
  vec_set(v, 0, (void *)99);
  EXPECT_EQ(vec_get(v, 0), (void *)99);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, SetSamePointerNoop) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  vec_push(v, (void *)1);
  vec_set(v, 0, (void *)1);  // 不应释放
  EXPECT_EQ(vec_get(v, 0), (void *)1);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, OwnModeFreesElements) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, true);
  vec_push(v, dup_int(a, 10));
  vec_push(v, dup_int(a, 20));
  // erase 应释放被删元素
  vec_erase(v, 0);
  EXPECT_EQ(vec_get_size(v), 1u);
  // delete_vec 释放剩余
  delete_vec(&v);
  delete_allocator(&a);
  SUCCEED();
}

TEST(VecTest, OwnModeSetFreesOld) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, true);
  vec_push(v, dup_int(a, 10));
  vec_set(v, 0, dup_int(a, 20));
  EXPECT_EQ(*(int *)vec_get(v, 0), 20);
  delete_vec(&v);
  delete_allocator(&a);
  SUCCEED();
}

TEST(VecTest, ReserveDoesNotShrinkSize) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  for (int i = 0; i < 10; i++) vec_push(v, (void *)(intptr_t)(i + 1));
  EXPECT_EQ(vec_get_size(v), 10u);
  vec_reserve(v, 5);
  EXPECT_EQ(vec_get_size(v), 10u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, ReserveGrowsCapacity) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  vec_reserve(v, 100);
  EXPECT_GE(vec_get_capacity(v), 100u);
  EXPECT_EQ(vec_get_size(v), 0u);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, ResizeGrowZeroFills) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  vec_resize(v, 5);
  EXPECT_EQ(vec_get_size(v), 5u);
  for (size_t i = 0; i < 5; i++) EXPECT_EQ(vec_get(v, i), nullptr);
  delete_vec(&v);
  delete_allocator(&a);
}

TEST(VecTest, ResizeShrinkFreesOwned) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, true);
  vec_push(v, dup_int(a, 1));
  vec_push(v, dup_int(a, 2));
  vec_push(v, dup_int(a, 3));
  vec_resize(v, 1);
  EXPECT_EQ(vec_get_size(v), 1u);
  EXPECT_EQ(*(int *)vec_get(v, 0), 1);
  delete_vec(&v);
  delete_allocator(&a);
  SUCCEED();
}

TEST(VecTest, PopEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  vec_t *v = create_vec(a, false);
  EXPECT_EQ(vec_pop(v), (size_t)-1);
  delete_vec(&v);
  delete_allocator(&a);
}
