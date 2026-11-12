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
  rbtree_t *t = create_rbtree(a);
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
  rbtree_t *t = create_rbtree(a);
  int v1 = 100, v2 = 200;
  EXPECT_EQ(rbtree_insert(t, 10, &v1), nullptr);
  EXPECT_EQ(rbtree_insert(t, 20, &v2), nullptr);
  EXPECT_EQ(rbtree_get_size(t), 2u);
  EXPECT_EQ(rbtree_find(t, 10), &v1);
  EXPECT_EQ(rbtree_find(t, 20), &v2);
  EXPECT_EQ(rbtree_find(t, 99), nullptr);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, InsertDuplicateKeyReplacesAndReturnsOldValue) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int v1 = 100, v2 = 200;
  EXPECT_EQ(rbtree_insert(t, 10, &v1), nullptr);
  EXPECT_EQ(rbtree_find(t, 10), &v1);
  EXPECT_EQ(rbtree_insert(t, 10, &v2), &v1);
  EXPECT_EQ(rbtree_find(t, 10), &v2);
  EXPECT_EQ(rbtree_get_size(t), 1u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, InsertNullValueReturnsError) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  EXPECT_EQ(rbtree_insert(t, 10, NULL), nullptr);
  EXPECT_EQ(rbtree_get_size(t), 0u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 插入顺序不敏感 ---- */

TEST(RbtreeTest, InsertAscendingOrder) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int vals[20];
  for (int i = 0; i < 20; i++) {
    vals[i] = i;
    EXPECT_EQ(rbtree_insert(t, (uintptr_t)i, &vals[i]), nullptr);
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
  rbtree_t *t = create_rbtree(a);
  int vals[20];
  for (int i = 19; i >= 0; i--) {
    vals[i] = i;
    EXPECT_EQ(rbtree_insert(t, (uintptr_t)i, &vals[i]), nullptr);
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
  rbtree_t *t = create_rbtree(a);
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
    EXPECT_EQ(rbtree_insert(t, (uintptr_t)vals[i], &vals[vals[i]]), nullptr);
  }
  EXPECT_EQ(rbtree_get_size(t), 50u);
  for (int i = 0; i < 50; i++) {
    EXPECT_EQ(rbtree_find(t, (uintptr_t)i), &vals[i]);
  }
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 删除 ---- */

TEST(RbtreeTest, EraseExistingReturnsValue) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int v1 = 100, v2 = 200, v3 = 300;
  rbtree_insert(t, 10, &v1);
  rbtree_insert(t, 20, &v2);
  rbtree_insert(t, 30, &v3);
  EXPECT_EQ(rbtree_erase(t, 20), &v2);
  EXPECT_EQ(rbtree_get_size(t), 2u);
  EXPECT_EQ(rbtree_find(t, 20), nullptr);
  EXPECT_EQ(rbtree_find(t, 10), &v1);
  EXPECT_EQ(rbtree_find(t, 30), &v3);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseNonexistentReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int v = 1;
  rbtree_insert(t, 10, &v);
  EXPECT_EQ(rbtree_erase(t, 99), nullptr);
  EXPECT_EQ(rbtree_get_size(t), 1u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseAll) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int vals[10];
  for (int i = 0; i < 10; i++) {
    vals[i] = i;
    rbtree_insert(t, (uintptr_t)i, &vals[i]);
  }
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(rbtree_erase(t, (uintptr_t)i), &vals[i]);
    EXPECT_EQ(rbtree_get_size(t), (size_t)(9 - i));
  }
  EXPECT_EQ(rbtree_get_size(t), 0u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseRoot) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int v1 = 1, v2 = 2, v3 = 3;
  rbtree_insert(t, 20, &v1);
  rbtree_insert(t, 10, &v2);
  rbtree_insert(t, 30, &v3);
  /* 删除根 */
  EXPECT_EQ(rbtree_erase(t, 20), &v1);
  EXPECT_EQ(rbtree_get_size(t), 2u);
  EXPECT_EQ(rbtree_find(t, 20), nullptr);
  EXPECT_EQ(rbtree_find(t, 10), &v2);
  EXPECT_EQ(rbtree_find(t, 30), &v3);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseAndReinsert) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int v = 42;
  rbtree_insert(t, 10, &v);
  EXPECT_EQ(rbtree_erase(t, 10), &v);
  EXPECT_EQ(rbtree_get_size(t), 0u);
  EXPECT_EQ(rbtree_insert(t, 10, &v), nullptr);
  EXPECT_EQ(rbtree_get_size(t), 1u);
  EXPECT_EQ(rbtree_find(t, 10), &v);
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 遍历（中序升序） ---- */

TEST(RbtreeTest, ForeachAscendingOrder) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
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
  rbtree_t *t = create_rbtree(a);
  struct collect_ctx ctx = {0};
  rbtree_foreach(t, collect_cb, &ctx);
  EXPECT_EQ(ctx.count, 0u);
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 调用方管理 value 生命周期 ---- */

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

TEST(RbtreeTest, CallerManagesReplacedAndRemovedValues) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  rbtree_t *t = create_rbtree(a);
  int *first = (int *)allocator_alloc(a, sizeof(int));
  int *second = (int *)allocator_alloc(a, sizeof(int));
  *first = 100;
  *second = 200;

  EXPECT_EQ(rbtree_insert(t, 10, first), nullptr);
  EXPECT_EQ(rbtree_insert(t, 10, second), first);
  allocator_free(a, (void **)&first);

  int *removed = (int *)rbtree_erase(t, 10);
  EXPECT_EQ(removed, second);
  allocator_free(a, (void **)&removed);

  delete_rbtree(&t);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}

TEST(RbtreeTest, DeleteTreeDoesNotFreeValues) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  rbtree_t *t = create_rbtree(a);
  int *value = (int *)allocator_alloc(a, sizeof(int));
  *value = 7;
  rbtree_insert(t, 10, value);
  delete_rbtree(&t);
  allocator_free(a, (void **)&value);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}

/* ---- 边界：空树操作 ---- */

TEST(RbtreeTest, FindOnEmptyTree) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  EXPECT_EQ(rbtree_find(t, 42), nullptr);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeTest, EraseOnEmptyTree) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  EXPECT_EQ(rbtree_erase(t, 42), nullptr);
  delete_rbtree(&t);
  delete_allocator(&a);
}

/* ---- 大规模插入/删除验证树平衡 ---- */

TEST(RbtreeTest, LargeInsertAndFindAll) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
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
    EXPECT_EQ(rbtree_erase(t, (uintptr_t)i), &vals[i]);
    EXPECT_EQ(rbtree_get_size(t), (size_t)(499 - i));
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
  rbtree_t *t = create_rbtree(a);
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

/* ---- 覆盖率补强：删除路径的对称分支 ----
   构造特定的树形以覆盖 delete_fixup 中 x 为右孩子的分支，
   以及 erase 中 z 只有左孩子 / 后继不是直接右孩子的情形。 */

TEST(RbtreeCoverageTest, EraseNodeWithOnlyLeftChild) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int v1 = 1, v2 = 2;
  /* 先插 20 再插 10：10 成为 20 的左孩子，20 无右孩子 */
  rbtree_insert(t, 20, &v1);
  rbtree_insert(t, 10, &v2);
  /* 删除 20（只有左孩子）→ 走 z->right == nil 分支 */
  EXPECT_EQ(rbtree_erase(t, 20), &v1);
  EXPECT_EQ(rbtree_get_size(t), 1u);
  EXPECT_EQ(rbtree_find(t, 20), nullptr);
  EXPECT_EQ(rbtree_find(t, 10), &v2);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeCoverageTest, EraseNodeWithSuccessorDeeperInRightSubtree) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  int vals[7];
  for (int i = 0; i < 7; i++)
    vals[i] = i;
  /* 构造：根 20，左 10，右 30；30 的左 25，25 的左 22 */
  rbtree_insert(t, 20, &vals[0]);
  rbtree_insert(t, 10, &vals[1]);
  rbtree_insert(t, 30, &vals[2]);
  rbtree_insert(t, 25, &vals[3]);
  rbtree_insert(t, 22, &vals[4]);
  /* 删除 20：后继是 22（不是直接右孩子 30）→ 覆盖 y->parent != z 分支 */
  EXPECT_EQ(rbtree_erase(t, 20), &vals[0]);
  EXPECT_EQ(rbtree_get_size(t), 4u);
  EXPECT_EQ(rbtree_find(t, 20), nullptr);
  EXPECT_EQ(rbtree_find(t, 22), &vals[4]);
  EXPECT_EQ(rbtree_find(t, 30), &vals[2]);
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeCoverageTest, EraseTriggersRightSideFixup) {
  allocator_t *a = create_allocator(malloc, free);
  rbtree_t *t = create_rbtree(a);
  static int vals[16];
  for (int i = 0; i < 16; i++)
    vals[i] = i;
  /* 顺序插入 1..15 形成左倾树，删除最小值会触发 x 为右孩子的修复分支 */
  for (int i = 1; i <= 15; i++) {
    rbtree_insert(t, (uintptr_t)i, &vals[i]);
  }
  for (int i = 1; i <= 7; i++) {
    EXPECT_EQ(rbtree_erase(t, (uintptr_t)i), &vals[i]);
  }
  EXPECT_EQ(rbtree_get_size(t), 8u);
  for (int i = 8; i <= 15; i++) {
    EXPECT_EQ(rbtree_find(t, (uintptr_t)i), &vals[i]);
  }
  delete_rbtree(&t);
  delete_allocator(&a);
}

TEST(RbtreeCoverageTest, EraseManyRandomOrders) {
  allocator_t *a = create_allocator(malloc, free);
  static int vals[128];
  for (int i = 0; i < 128; i++)
    vals[i] = i;
  /* 多种插入/删除顺序，尽可能覆盖 delete_fixup 的所有对称分支 */
  unsigned seeds[] = {1, 7, 13, 42, 99, 123, 777, 2024};
  for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
    rbtree_t *t = create_rbtree(a);
    int order[128];
    for (int i = 0; i < 128; i++)
      order[i] = i;
    srand(seeds[s]);
    for (int i = 127; i > 0; i--) {
      int j = rand() % (i + 1);
      int tmp = order[i];
      order[i] = order[j];
      order[j] = tmp;
    }
    for (int i = 0; i < 128; i++) {
      rbtree_insert(t, (uintptr_t)order[i], &vals[order[i]]);
    }
    /* 用另一个打乱顺序删除全部 */
    for (int i = 127; i > 0; i--) {
      int j = rand() % (i + 1);
      int tmp = order[i];
      order[i] = order[j];
      order[j] = tmp;
    }
    for (int i = 0; i < 128; i++) {
      EXPECT_NE(rbtree_erase(t, (uintptr_t)order[i]), nullptr);
    }
    EXPECT_EQ(rbtree_get_size(t), 0u);
    delete_rbtree(&t);
  }
  delete_allocator(&a);
}
