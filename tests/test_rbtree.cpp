#include <gtest/gtest.h>
#include "core/rbtree.h"
#include "core/allocator.h"
#include <stdlib.h>
#include <stdint.h>

/* ---- 遍历收集器 ---- */
struct collect_ctx {
  uintptr_t keys[256];
  void *values[256];
  size_t count;
};

static void collect_cb(uintptr_t key, void *value, void *user_data) {
  struct collect_ctx *ctx = (struct collect_ctx *)user_data;
  ctx->keys[ctx->count] = key;
  ctx->values[ctx->count] = value;
  ctx->count++;
}

/* ---- 基本生命周期 ---- */

TEST(RbtreeTest, CreateAndDelete) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  EXPECT_NE(t, nullptr);
  EXPECT_EQ(rbtree_get_size(t), 0u);
  delete_rbtree(&t);
  EXPECT_EQ(t, nullptr);
  delete_allocator(&a);
}

TEST(RbtreeTest, DeleteNullIsNoop) {
  rbtree_t *t = nullptr;
  delete_rbtree(&t);
  EXPECT_EQ(t, nullptr);
  delete_rbtree(nullptr);
}

/* ---- 插入与查找 ---- */

TEST(RbtreeTest, InsertAndFind) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int v1 = 100, v2 = 200;
  EXPECT_EQ(rbtree_insert(t, 10, &v1), 1u);
  EXPECT_EQ(rbtree_insert(t, 20, &v2), 2u);
  EXPECT_EQ(rbtree_find(t, 10), &v1);
  EXPECT_EQ(rbtree_find(t, 20), &v2);
  EXPECT_EQ(rbtree_find(t, 99), nullptr);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, InsertDuplicateKeyReplacesValue) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int v1 = 100, v2 = 200;
  rbtree_insert(t, 10, &v1);
  EXPECT_EQ(rbtree_find(t, 10), &v1);
  /* 重复 key 替换 */
  rbtree_insert(t, 10, &v2);
  EXPECT_EQ(rbtree_find(t, 10), &v2);
  EXPECT_EQ(rbtree_get_size(t), 1u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, InsertNullValueReturnsError) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  EXPECT_EQ(rbtree_insert(t, 10, NULL), (size_t)-1);
  EXPECT_EQ(rbtree_get_size(t), 0u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 插入顺序不敏感 ---- */

TEST(RbtreeTest, InsertAscendingOrder) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int vals[20];
  for (int i = 0; i < 20; i++) {
    vals[i] = i;
    rbtree_insert(t, (uintptr_t)i, &vals[i]);
  }
  EXPECT_EQ(rbtree_get_size(t), 20u);
  for (int i = 0; i < 20; i++) {
    EXPECT_EQ(rbtree_find(t, (uintptr_t)i), &vals[i]);
  }
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, InsertDescendingOrder) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int vals[20];
  for (int i = 19; i >= 0; i--) {
    vals[i] = i;
    rbtree_insert(t, (uintptr_t)i, &vals[i]);
  }
  EXPECT_EQ(rbtree_get_size(t), 20u);
  for (int i = 0; i < 20; i++) {
    EXPECT_EQ(rbtree_find(t, (uintptr_t)i), &vals[i]);
  }
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, InsertRandomOrder) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int vals[50];
  /* 固定种子保证可重现 */
  srand(42);
  for (int i = 0; i < 50; i++) {
    vals[i] = i;
  }
  /* 打乱插入顺序 */
  for (int i = 49; i > 0; i--) {
    int j = rand() % (i + 1);
    int tmp = vals[i];
    vals[i] = vals[j];
    vals[j] = tmp;
  }
  for (int i = 0; i < 50; i++) {
    rbtree_insert(t, (uintptr_t)vals[i], &vals[vals[i]]);
  }
  EXPECT_EQ(rbtree_get_size(t), 50u);
  for (int i = 0; i < 50; i++) {
    EXPECT_EQ(rbtree_find(t, (uintptr_t)i), &vals[i]);
  }
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 删除 ---- */

TEST(RbtreeTest, EraseExisting) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int v1 = 100, v2 = 200, v3 = 300;
  rbtree_insert(t, 10, &v1);
  rbtree_insert(t, 20, &v2);
  rbtree_insert(t, 30, &v3);
  EXPECT_EQ(rbtree_erase(t, 20), 2u);
  EXPECT_EQ(rbtree_find(t, 20), nullptr);
  EXPECT_EQ(rbtree_find(t, 10), &v1);
  EXPECT_EQ(rbtree_find(t, 30), &v3);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseNonexistentReturnsError) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int v = 1;
  rbtree_insert(t, 10, &v);
  EXPECT_EQ(rbtree_erase(t, 99), (size_t)-1);
  EXPECT_EQ(rbtree_get_size(t), 1u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseAll) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int vals[10];
  for (int i = 0; i < 10; i++) {
    vals[i] = i;
    rbtree_insert(t, (uintptr_t)i, &vals[i]);
  }
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(rbtree_erase(t, (uintptr_t)i), (size_t)(9 - i));
  }
  EXPECT_EQ(rbtree_get_size(t), 0u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseRoot) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int v1 = 1, v2 = 2, v3 = 3;
  rbtree_insert(t, 20, &v1);
  rbtree_insert(t, 10, &v2);
  rbtree_insert(t, 30, &v3);
  /* 删除根 */
  EXPECT_EQ(rbtree_erase(t, 20), 2u);
  EXPECT_EQ(rbtree_find(t, 20), nullptr);
  EXPECT_EQ(rbtree_find(t, 10), &v2);
  EXPECT_EQ(rbtree_find(t, 30), &v3);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseAndReinsert) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int v = 42;
  rbtree_insert(t, 10, &v);
  rbtree_erase(t, 10);
  EXPECT_EQ(rbtree_get_size(t), 0u);
  rbtree_insert(t, 10, &v);
  EXPECT_EQ(rbtree_get_size(t), 1u);
  EXPECT_EQ(rbtree_find(t, 10), &v);
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 遍历（中序升序） ---- */

TEST(RbtreeTest, ForeachAscendingOrder) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int vals[10];
  srand(123);
  for (int i = 0; i < 10; i++)
    vals[i] = i;
  for (int i = 9; i > 0; i--) {
    int j = rand() % (i + 1);
    int tmp = vals[i];
    vals[i] = vals[j];
    vals[j] = tmp;
  }
  for (int i = 0; i < 10; i++) {
    rbtree_insert(t, (uintptr_t)vals[i], &vals[vals[i]]);
  }
  struct collect_ctx ctx = {0};
  rbtree_foreach(t, collect_cb, &ctx);
  ASSERT_EQ(ctx.count, 10u);
  /* 验证 key 升序 */
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(ctx.keys[i], (uintptr_t)i);
  }
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, ForeachEmptyTree) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  struct collect_ctx ctx = {0};
  rbtree_foreach(t, collect_cb, &ctx);
  EXPECT_EQ(ctx.count, 0u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- own 模式 ---- */

static void *dup_int(allocator_t *a, int v) {
  int *p = (int *)allocator_alloc(a, sizeof(int));
  *p = v;
  return p;
}

TEST(RbtreeTest, OwnModeFreesValuesOnDelete) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, true);
  for (int i = 0; i < 10; i++) {
    rbtree_insert(t, (uintptr_t)i, dup_int(a, i));
  }
  /* delete_rbtree 释放所有 value */
  delete_rbtree(&t);
  delete_allocator(&a);
  SUCCEED();
}

TEST(RbtreeTest, OwnModeFreesOldValueOnDuplicate) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, true);
  rbtree_insert(t, 10, dup_int(a, 100));
  /* 重复 key 应释放旧值 */
  rbtree_insert(t, 10, dup_int(a, 200));
  EXPECT_EQ(*(int *)rbtree_find(t, 10), 200);
  EXPECT_EQ(rbtree_get_size(t), 1u);
  delete_rbtree(&t);
  delete_allocator(&a);
  SUCCEED();
}

TEST(RbtreeTest, OwnModeFreesValueOnErase) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, true);
  rbtree_insert(t, 10, dup_int(a, 100));
  rbtree_insert(t, 20, dup_int(a, 200));
  rbtree_erase(t, 10);
  EXPECT_EQ(rbtree_find(t, 10), nullptr);
  EXPECT_EQ(*(int *)rbtree_find(t, 20), 200);
  delete_rbtree(&t);
  delete_allocator(&a);
  SUCCEED();
}

/* ---- 边界：空树操作 ---- */

TEST(RbtreeTest, FindOnEmptyTree) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  EXPECT_EQ(rbtree_find(t, 42), nullptr);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseOnEmptyTree) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  EXPECT_EQ(rbtree_erase(t, 42), (size_t)-1);
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 大规模插入/删除验证树平衡 ---- */

TEST(RbtreeTest, LargeInsertAndFindAll) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  static int vals[500];
  for (int i = 0; i < 500; i++) {
    vals[i] = i;
  }
  srand(777);
  for (int i = 499; i > 0; i--) {
    int j = rand() % (i + 1);
    int tmp = vals[i];
    vals[i] = vals[j];
    vals[j] = tmp;
  }
  for (int i = 0; i < 500; i++) {
    rbtree_insert(t, (uintptr_t)vals[i], &vals[vals[i]]);
  }
  EXPECT_EQ(rbtree_get_size(t), 500u);
  for (int i = 0; i < 500; i++) {
    EXPECT_EQ(rbtree_find(t, (uintptr_t)i), &vals[i]);
  }
  /* 删除一半 */
  for (int i = 0; i < 250; i++) {
    EXPECT_EQ(rbtree_erase(t, (uintptr_t)i), (size_t)(499 - i));
  }
  EXPECT_EQ(rbtree_get_size(t), 250u);
  /* 剩余仍可查 */
  for (int i = 250; i < 500; i++) {
    EXPECT_EQ(rbtree_find(t, (uintptr_t)i), &vals[i]);
  }
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 遍历验证删除后仍升序 ---- */

TEST(RbtreeTest, ForeachAfterEraseStillAscending) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a, false);
  int vals[20];
  for (int i = 0; i < 20; i++) {
    vals[i] = i;
    rbtree_insert(t, (uintptr_t)i, &vals[i]);
  }
  /* 删除偶数 key */
  for (int i = 0; i < 20; i += 2) {
    rbtree_erase(t, (uintptr_t)i);
  }
  struct collect_ctx ctx = {0};
  rbtree_foreach(t, collect_cb, &ctx);
  ASSERT_EQ(ctx.count, 10u);
  /* 剩余应为奇数 1,3,5,...,19 */
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(ctx.keys[i], (uintptr_t)(2 * i + 1));
  }
  delete_rbtree(&t);
  delete_allocator(&a);
}
