#include <gtest/gtest.h>
#include "test_common.h"
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/strmap.h"
#include "core/allocator.h"
}

/* ---- Test helpers ---- */

static allocator_t *g_alloc = nullptr;

class StrmapTest : public ::testing::Test {
protected:
  void SetUp() override { g_alloc = create_allocator(malloc, free); }
  void TearDown() override { EXPECT_ALLOCATOR_EMPTY_DELETE(&g_alloc); }
};

/* A simple boxed value for owns_value testing */
typedef struct {
  int x;
} box_t;

static class_t box_class = {
    .name = "box_t",
    .size = sizeof(box_t),
    .move_fn = default_move,
    .clone_fn = default_clone,
    .dispose_fn = nullptr,
};

static box_t *box_new(allocator_t *alloc, int x) {
  box_t *b = (box_t *)allocator_new(alloc, &box_class, 1);
  b->x = x;
  return b;
}

/* ---- Construction / destruction ---- */

TEST_F(StrmapTest, NewAndFree) {
  strmap_t *m = strmap_new(g_alloc, false);
  ASSERT_NE(m, nullptr);
  EXPECT_TRUE(strmap_is_empty(m));
  EXPECT_EQ(strmap_size(m), 0u);
  strmap_free(g_alloc, &m);
  EXPECT_EQ(m, nullptr);
}

TEST_F(StrmapTest, NullArgs) {
  EXPECT_EQ(strmap_new(nullptr, false), nullptr);
  strmap_free(nullptr, nullptr);
  strmap_free(g_alloc, nullptr);
}

/* ---- Insert ---- */

TEST_F(StrmapTest, InsertAndGet) {
  strmap_t *m = strmap_new(g_alloc, false);

  void *old = strmap_insert(m, g_alloc, "hello", (void *)1);
  EXPECT_EQ(old, nullptr);
  EXPECT_EQ(strmap_size(m), 1u);

  EXPECT_EQ(strmap_get(m, "hello"), (void *)1);
  EXPECT_EQ(strmap_get(m, "world"), nullptr);

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, InsertReplace) {
  strmap_t *m = strmap_new(g_alloc, false);

  strmap_insert(m, g_alloc, "key", (void *)10);
  void *old = strmap_insert(m, g_alloc, "key", (void *)20);
  EXPECT_EQ(old, (void *)10);
  EXPECT_EQ(strmap_size(m), 1u);
  EXPECT_EQ(strmap_get(m, "key"), (void *)20);

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, InsertMultiple) {
  strmap_t *m = strmap_new(g_alloc, false);

  strmap_insert(m, g_alloc, "alpha", (void *)1);
  strmap_insert(m, g_alloc, "beta", (void *)2);
  strmap_insert(m, g_alloc, "gamma", (void *)3);

  EXPECT_EQ(strmap_size(m), 3u);
  EXPECT_EQ(strmap_get(m, "alpha"), (void *)1);
  EXPECT_EQ(strmap_get(m, "beta"), (void *)2);
  EXPECT_EQ(strmap_get(m, "gamma"), (void *)3);

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, InsertNullKeyNoop) {
  strmap_t *m = strmap_new(g_alloc, false);
  void *result = strmap_insert(m, g_alloc, nullptr, (void *)1);
  EXPECT_EQ(result, nullptr);
  EXPECT_TRUE(strmap_is_empty(m));
  strmap_free(g_alloc, &m);
}

/* ---- Contains ---- */

TEST_F(StrmapTest, Contains) {
  strmap_t *m = strmap_new(g_alloc, false);
  strmap_insert(m, g_alloc, "yes", (void *)1);

  EXPECT_TRUE(strmap_contains(m, "yes"));
  EXPECT_FALSE(strmap_contains(m, "no"));
  EXPECT_FALSE(strmap_contains(m, nullptr));

  strmap_free(g_alloc, &m);
}

/* ---- Remove ---- */

TEST_F(StrmapTest, RemoveExisting) {
  strmap_t *m = strmap_new(g_alloc, false);

  strmap_insert(m, g_alloc, "a", (void *)100);
  strmap_insert(m, g_alloc, "b", (void *)200);

  void *val = strmap_remove(m, "a");
  EXPECT_EQ(val, (void *)100);
  EXPECT_EQ(strmap_size(m), 1u);
  EXPECT_FALSE(strmap_contains(m, "a"));
  EXPECT_TRUE(strmap_contains(m, "b"));

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, RemoveNonExistent) {
  strmap_t *m = strmap_new(g_alloc, false);
  strmap_insert(m, g_alloc, "a", (void *)1);

  void *val = strmap_remove(m, "nope");
  EXPECT_EQ(val, nullptr);
  EXPECT_EQ(strmap_size(m), 1u);

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, RemoveNullNoop) {
  strmap_t *m = strmap_new(g_alloc, false);
  void *val = strmap_remove(m, nullptr);
  EXPECT_EQ(val, nullptr);
  strmap_free(g_alloc, &m);
}

/* ---- Key order ---- */

TEST_F(StrmapTest, KeysPreserveInsertionOrder) {
  strmap_t *m = strmap_new(g_alloc, false);

  strmap_insert(m, g_alloc, "charlie", (void *)3);
  strmap_insert(m, g_alloc, "alpha", (void *)1);
  strmap_insert(m, g_alloc, "bravo", (void *)2);

  const vec_t *keys = strmap_keys(m);
  ASSERT_NE(keys, nullptr);
  ASSERT_EQ(vec_len(keys), 3u);
  /* Insertion order, not sorted order */
  EXPECT_STREQ((const char *)vec_get(keys, 0), "charlie");
  EXPECT_STREQ((const char *)vec_get(keys, 1), "alpha");
  EXPECT_STREQ((const char *)vec_get(keys, 2), "bravo");

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, RemovePreservesOrder) {
  strmap_t *m = strmap_new(g_alloc, false);

  strmap_insert(m, g_alloc, "a", (void *)1);
  strmap_insert(m, g_alloc, "b", (void *)2);
  strmap_insert(m, g_alloc, "c", (void *)3);

  strmap_remove(m, "b");

  const vec_t *keys = strmap_keys(m);
  ASSERT_EQ(vec_len(keys), 2u);
  EXPECT_STREQ((const char *)vec_get(keys, 0), "a");
  EXPECT_STREQ((const char *)vec_get(keys, 1), "c");

  strmap_free(g_alloc, &m);
}

/* ---- Key ownership ---- */

TEST_F(StrmapTest, KeyOwnedCopyIndependent) {
  strmap_t *m = strmap_new(g_alloc, false);

  char key[] = "mutable";
  strmap_insert(m, g_alloc, key, (void *)1);

  /* Modify the original — the key inside the map should be independent */
  key[0] = 'M';
  EXPECT_TRUE(strmap_contains(m, "mutable"));
  EXPECT_FALSE(strmap_contains(m, "Mutable"));

  strmap_free(g_alloc, &m);
}

/* ---- owns_value ---- */

TEST_F(StrmapTest, OwnsValueFreed) {
  strmap_t *m = strmap_new(g_alloc, true);

  box_t *b1 = box_new(g_alloc, 42);
  box_t *b2 = box_new(g_alloc, 99);

  strmap_insert(m, g_alloc, "first", b1);
  strmap_insert(m, g_alloc, "second", b2);

  /* strmap_free with owns_value=true should free both box_t */
  strmap_free(g_alloc, &m);
  /* No ASAN/leak = success */
}

TEST_F(StrmapTest, OwnsValueReplaceFreesOld) {
  strmap_t *m = strmap_new(g_alloc, true);

  box_t *b1 = box_new(g_alloc, 10);
  box_t *b2 = box_new(g_alloc, 20);

  strmap_insert(m, g_alloc, "key", b1);
  void *old = strmap_insert(m, g_alloc, "key", b2);

  /* Old value is returned; caller is responsible for it */
  EXPECT_EQ(old, b1);
  allocator_free(g_alloc, &old);

  /* b2 is now in the map */
  EXPECT_EQ(strmap_get(m, "key"), b2);

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, OwnsValueRemoveTransfersOwnership) {
  strmap_t *m = strmap_new(g_alloc, true);

  box_t *b = box_new(g_alloc, 55);
  strmap_insert(m, g_alloc, "key", b);

  void *val = strmap_remove(m, "key");
  EXPECT_EQ(val, b);
  /* Caller owns the value now; must free it */
  allocator_free(g_alloc, &val);

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, OwnsValueClone) {
  strmap_t *m = strmap_new(g_alloc, true);

  box_t *b = box_new(g_alloc, 77);
  strmap_insert(m, g_alloc, "key", b);

  /* Clone the map */
  strmap_t *clone = (strmap_t *)allocator_clone(g_alloc, (void **)&m);
  ASSERT_NE(clone, nullptr);

  /* Clone has its copy of the value */
  box_t *clone_val = (box_t *)strmap_get(clone, "key");
  ASSERT_NE(clone_val, nullptr);
  EXPECT_NE(clone_val, b);     /* different pointer */
  EXPECT_EQ(clone_val->x, 77); /* same value */

  /* Free original; clone's value should be independent */
  strmap_free(g_alloc, &m);

  clone_val = (box_t *)strmap_get(clone, "key");
  ASSERT_NE(clone_val, nullptr);
  EXPECT_EQ(clone_val->x, 77);

  strmap_free(g_alloc, &clone);
}

TEST_F(StrmapTest, NoOwnsValueNotFreed) {
  strmap_t *m = strmap_new(g_alloc, false);

  box_t *b = box_new(g_alloc, 42);
  strmap_insert(m, g_alloc, "key", b);

  /* strmap_free with owns_value=false does NOT free b */
  strmap_free(g_alloc, &m);

  /* b is still valid; caller frees it */
  EXPECT_EQ(b->x, 42);
  allocator_free(g_alloc, (void **)&b);
}

/* ---- Clone without owns_value ---- */

TEST_F(StrmapTest, CloneNoOwnsValue) {
  strmap_t *m = strmap_new(g_alloc, false);

  strmap_insert(m, g_alloc, "a", (void *)1);
  strmap_insert(m, g_alloc, "b", (void *)2);

  strmap_t *clone = (strmap_t *)allocator_clone(g_alloc, (void **)&m);
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(strmap_size(clone), 2u);
  EXPECT_EQ(strmap_get(clone, "a"), (void *)1);
  EXPECT_EQ(strmap_get(clone, "b"), (void *)2);

  /* Keys are independent copies */
  EXPECT_TRUE(strmap_contains(clone, "a"));

  strmap_free(g_alloc, &m);
  strmap_free(g_alloc, &clone);
}

/* ---- Move ---- */

TEST_F(StrmapTest, Move) {
  strmap_t *m = strmap_new(g_alloc, false);

  strmap_insert(m, g_alloc, "x", (void *)10);
  strmap_insert(m, g_alloc, "y", (void *)20);

  strmap_t *moved = (strmap_t *)allocator_move(g_alloc, (void **)&m);
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(m, nullptr);

  EXPECT_EQ(strmap_size(moved), 2u);
  EXPECT_EQ(strmap_get(moved, "x"), (void *)10);
  EXPECT_EQ(strmap_get(moved, "y"), (void *)20);

  strmap_free(g_alloc, &moved);
}

/* ---- Edge cases ---- */

TEST_F(StrmapTest, EmptyStringKey) {
  strmap_t *m = strmap_new(g_alloc, false);

  strmap_insert(m, g_alloc, "", (void *)42);
  EXPECT_TRUE(strmap_contains(m, ""));
  EXPECT_EQ(strmap_get(m, ""), (void *)42);

  void *val = strmap_remove(m, "");
  EXPECT_EQ(val, (void *)42);
  EXPECT_TRUE(strmap_is_empty(m));

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, LongKey) {
  strmap_t *m = strmap_new(g_alloc, false);

  std::string long_key(1000, 'x');
  strmap_insert(m, g_alloc, long_key.c_str(), (void *)1);
  EXPECT_TRUE(strmap_contains(m, long_key.c_str()));

  strmap_free(g_alloc, &m);
}

TEST_F(StrmapTest, NullMapAccessors) {
  EXPECT_EQ(strmap_size(nullptr), 0u);
  EXPECT_TRUE(strmap_is_empty(nullptr));
  EXPECT_EQ(strmap_get(nullptr, "key"), nullptr);
  EXPECT_FALSE(strmap_contains(nullptr, "key"));
  EXPECT_EQ(strmap_keys(nullptr), nullptr);
  EXPECT_FALSE(strmap_owns_value(nullptr));
}
