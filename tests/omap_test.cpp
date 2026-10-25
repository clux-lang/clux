#include <gtest/gtest.h>
#include "test_common.h"
#include <string>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/omap.h"
}

/* ---- Test allocator helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr) { free(ptr); }

/* ---- Comparison functions ---- */

static int cmp_int(const void *a, const void *b) {
  int ia = *(const int *)a;
  int ib = *(const int *)b;
  return (ia > ib) - (ia < ib);
}

/* ---- int_box class ---- */

typedef struct {
  int value;
} int_box_t;

/* ---- str_box class ---- */

static class_t byte_class = {
    .name = "byte",
    .size = 1,
    .move_fn = default_move,
    .clone_fn = default_clone,
    .dispose_fn = NULL,
};

typedef struct {
  char *str;
} str_box_t;

static void str_box_clone(void *self, allocator_t *allocator, void *another) {
  str_box_t *dst = (str_box_t *)self;
  str_box_t *src = (str_box_t *)another;
  size_t len = strlen(src->str) + 1;
  dst->str = (char *)allocator_new(allocator, &byte_class, len);
  memcpy(dst->str, src->str, len);
}

static void str_box_dispose(void *self, allocator_t *allocator) {
  str_box_t *obj = (str_box_t *)self;
  if (obj->str) {
    void *p = obj->str;
    allocator_free(allocator, &p);
    obj->str = NULL;
  }
}

static void str_box_move(void *self, allocator_t *allocator, void *another) {
  (void)allocator;
  str_box_t *dst = (str_box_t *)self;
  str_box_t *src = (str_box_t *)another;
  dst->str = src->str;
  src->str = NULL;
}

static class_t str_box_class = {
    .name = "str_box",
    .size = sizeof(str_box_t),
    .move_fn = str_box_move,
    .clone_fn = str_box_clone,
    .dispose_fn = str_box_dispose,
};

static int cmp_str_box(const void *a, const void *b) {
  return strcmp(((const str_box_t *)a)->str, ((const str_box_t *)b)->str);
}

/* ==== OMapNew / OMapFree ==== */

TEST(OMapNew, EmptyMap) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(omap_size(m), 0u);
  EXPECT_TRUE(omap_is_empty(m));
  EXPECT_FALSE(omap_owns_key(m));
  EXPECT_FALSE(omap_owns_value(m));
  omap_free(a, &m);
  EXPECT_EQ(m, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(OMapNew, NullArgs) {
  EXPECT_EQ(omap_new(NULL, cmp_int, false, false), nullptr);
  EXPECT_EQ(
      omap_new(create_allocator(test_alloc, test_free), NULL, false, false),
      nullptr);
}

TEST(OMapFree, NullSafe) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_free(a, nullptr);
  omap_t *null_m = nullptr;
  omap_free(a, &null_m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Insert / Get / Contains ==== */

TEST(OMapInsert, BasicInsertAndGet) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  int k1 = 1, v1 = 10;
  int k2 = 2, v2 = 20;
  int k3 = 3, v3 = 30;

  EXPECT_EQ(omap_insert(m, a, &k1, &v1), nullptr);
  EXPECT_EQ(omap_insert(m, a, &k2, &v2), nullptr);
  EXPECT_EQ(omap_insert(m, a, &k3, &v3), nullptr);

  EXPECT_EQ(omap_size(m), 3u);
  EXPECT_EQ(*(int *)omap_get(m, &k1), 10);
  EXPECT_EQ(*(int *)omap_get(m, &k2), 20);
  EXPECT_EQ(*(int *)omap_get(m, &k3), 30);
  EXPECT_TRUE(omap_contains(m, &k2));

  int k_missing = 99;
  EXPECT_EQ(omap_get(m, &k_missing), nullptr);
  EXPECT_FALSE(omap_contains(m, &k_missing));

  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(OMapInsert, DuplicateKeyReplaceValue) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  int k = 42, v1 = 100, v2 = 200;
  omap_insert(m, a, &k, &v1);
  void *old = omap_insert(m, a, &k, &v2);
  EXPECT_EQ(*(int *)old, 100);
  EXPECT_EQ(omap_size(m), 1u);
  EXPECT_EQ(*(int *)omap_get(m, &k), 200);

  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Insertion order preserved in key_vec ==== */

TEST(OMapKeys, InsertionOrderPreserved) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  int keys[] = {5, 3, 7, 1, 9};
  int vals[] = {50, 30, 70, 10, 90};
  for (int i = 0; i < 5; i++)
    omap_insert(m, a, &keys[i], &vals[i]);

  const vec_t *kvec = omap_keys(m);
  ASSERT_NE(kvec, nullptr);
  ASSERT_EQ(vec_len(kvec), 5u);
  /* Order should be insertion order, not sorted */
  EXPECT_EQ(*(int *)vec_get(kvec, 0), 5);
  EXPECT_EQ(*(int *)vec_get(kvec, 1), 3);
  EXPECT_EQ(*(int *)vec_get(kvec, 2), 7);
  EXPECT_EQ(*(int *)vec_get(kvec, 3), 1);
  EXPECT_EQ(*(int *)vec_get(kvec, 4), 9);

  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Remove ==== */

TEST(OMapRemove, BasicRemove) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  int k1 = 1, v1 = 10;
  int k2 = 2, v2 = 20;
  omap_insert(m, a, &k1, &v1);
  omap_insert(m, a, &k2, &v2);

  void *removed = omap_remove(m, &k1);
  EXPECT_EQ(*(int *)removed, 10);
  EXPECT_EQ(omap_size(m), 1u);
  EXPECT_FALSE(omap_contains(m, &k1));
  EXPECT_TRUE(omap_contains(m, &k2));

  /* key_vec should no longer contain k1 */
  const vec_t *kvec = omap_keys(m);
  ASSERT_EQ(vec_len(kvec), 1u);
  EXPECT_EQ(*(int *)vec_get(kvec, 0), 2);

  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(OMapRemove, NotFound) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  int k = 1, v = 10;
  omap_insert(m, a, &k, &v);

  int missing = 99;
  EXPECT_EQ(omap_remove(m, &missing), nullptr);
  EXPECT_EQ(omap_size(m), 1u);

  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Owned key/value ==== */

TEST(OMapOwned, OwnsKeyAndValue) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_str_box, true, true);

  for (const char *s : {"alpha", "beta", "gamma"}) {
    str_box_t *kbox = (str_box_t *)allocator_new(a, &str_box_class, 1);
    size_t len = strlen(s) + 1;
    kbox->str = (char *)allocator_new(a, &byte_class, len);
    memcpy(kbox->str, s, len);

    str_box_t *vbox = (str_box_t *)allocator_new(a, &str_box_class, 1);
    vbox->str = (char *)allocator_new(a, &byte_class, len);
    memcpy(vbox->str, s, len);

    omap_insert(m, a, kbox, vbox);
  }

  EXPECT_EQ(omap_size(m), 3u);

  str_box_t search;
  search.str = (char *)"beta";
  str_box_t *found = (str_box_t *)omap_get(m, &search);
  ASSERT_NE(found, nullptr);
  EXPECT_STREQ(found->str, "beta");

  /* omap_free with owns=true frees all keys and values */
  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(OMapOwned, NonOwnedNotFreed) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  int k = 42, v = 100;
  omap_insert(m, a, &k, &v);

  omap_free(a, &m);
  /* k and v still valid */
  EXPECT_EQ(k, 42);
  EXPECT_EQ(v, 100);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Remove with owns_key ==== */

TEST(OMapOwned, RemoveOwnedKeyFreed) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_str_box, true, false);

  str_box_t *kbox = (str_box_t *)allocator_new(a, &str_box_class, 1);
  kbox->str = (char *)allocator_new(a, &byte_class, 6);
  memcpy(kbox->str, "hello", 6);

  int v = 42;
  omap_insert(m, a, kbox, &v);
  EXPECT_EQ(omap_size(m), 1u);

  /* Remove: key is freed (owns_key=true), value is returned */
  str_box_t search;
  search.str = (char *)"hello";
  void *removed_val = omap_remove(m, &search);
  EXPECT_EQ(*(int *)removed_val, 42);
  EXPECT_EQ(omap_size(m), 0u);

  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Clone ==== */

TEST(OMapClone, ShallowClone) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  int k = 42, v = 100;
  omap_insert(m, a, &k, &v);

  void *m_ptr = m;
  omap_t *cloned = (omap_t *)allocator_clone(a, &m_ptr);
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(omap_size(cloned), 1u);

  /* Shallow: same pointers */
  EXPECT_EQ(omap_get(cloned, &k), omap_get(m, &k));

  omap_free(a, &cloned);
  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(OMapClone, DeepClone) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_str_box, true, true);

  for (const char *s : {"aaa", "bbb"}) {
    str_box_t *kbox = (str_box_t *)allocator_new(a, &str_box_class, 1);
    size_t len = strlen(s) + 1;
    kbox->str = (char *)allocator_new(a, &byte_class, len);
    memcpy(kbox->str, s, len);

    str_box_t *vbox = (str_box_t *)allocator_new(a, &str_box_class, 1);
    vbox->str = (char *)allocator_new(a, &byte_class, len);
    memcpy(vbox->str, s, len);

    omap_insert(m, a, kbox, vbox);
  }

  void *m_ptr = m;
  omap_t *cloned = (omap_t *)allocator_clone(a, &m_ptr);
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(omap_size(cloned), 2u);

  /* Deep: different pointers, same content */
  str_box_t search;
  search.str = (char *)"aaa";
  str_box_t *orig_v = (str_box_t *)omap_get(m, &search);
  str_box_t *clone_v = (str_box_t *)omap_get(cloned, &search);
  ASSERT_NE(orig_v, nullptr);
  ASSERT_NE(clone_v, nullptr);
  EXPECT_NE(orig_v, clone_v);
  EXPECT_STREQ(orig_v->str, clone_v->str);
  EXPECT_NE(orig_v->str, clone_v->str);

  /* Key vec preserved insertion order in clone */
  const vec_t *ck = omap_keys(cloned);
  ASSERT_EQ(vec_len(ck), 2u);

  omap_free(a, &cloned);
  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Move ==== */

TEST(OMapMove, TransferOwnership) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  int k = 42, v = 100;
  omap_insert(m, a, &k, &v);

  omap_t *moved = (omap_t *)allocator_move(a, (void **)&m);
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(m, nullptr);
  EXPECT_EQ(omap_size(moved), 1u);
  EXPECT_EQ(*(int *)omap_get(moved, &k), 100);

  omap_free(a, &moved);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Stress test ==== */

TEST(OMapStress, ManyInsertsAndRemoves) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  omap_t *m = omap_new(a, cmp_int, false, false);

  const int N = 200;
  std::vector<int> keys(N), vals(N);
  for (int i = 0; i < N; i++) {
    keys[i] = i;
    vals[i] = i * 10;
  }

  for (int i = 0; i < N; i++)
    omap_insert(m, a, &keys[i], &vals[i]);
  EXPECT_EQ(omap_size(m), (size_t)N);

  /* Remove even keys */
  for (int i = 0; i < N; i += 2)
    omap_remove(m, &keys[i]);
  EXPECT_EQ(omap_size(m), (size_t)(N / 2));

  /* Verify odd keys still present */
  for (int i = 1; i < N; i += 2)
    EXPECT_TRUE(omap_contains(m, &keys[i]));

  /* Verify even keys absent */
  for (int i = 0; i < N; i += 2)
    EXPECT_FALSE(omap_contains(m, &keys[i]));

  /* Key vec should have only odd keys in original relative order */
  const vec_t *kvec = omap_keys(m);
  ASSERT_EQ(vec_len(kvec), (size_t)(N / 2));
  for (size_t i = 0; i < vec_len(kvec); i++) {
    int *kp = (int *)vec_get(kvec, i);
    EXPECT_EQ(*kp % 2, 1); /* all odd */
  }

  omap_free(a, &m);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Null safety ==== */

TEST(OMapNull, NullSafe) {
  EXPECT_EQ(omap_size(nullptr), 0u);
  EXPECT_TRUE(omap_is_empty(nullptr));
  EXPECT_EQ(omap_get(nullptr, (void *)1), nullptr);
  EXPECT_FALSE(omap_contains(nullptr, (void *)1));
  EXPECT_EQ(omap_remove(nullptr, (void *)1), nullptr);
  EXPECT_EQ(omap_keys(nullptr), nullptr);
  EXPECT_FALSE(omap_owns_key(nullptr));
  EXPECT_FALSE(omap_owns_value(nullptr));
}
