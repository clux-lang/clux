#include <gtest/gtest.h>
#include "test_common.h"
#include <stdexcept>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "core/allocator.h"
#include "core/panic.h"
}

/* ---- Test helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr) { free(ptr); }

/* Tracking allocator for verifying alloc/free call counts */
struct TrackState {
  int alloc_count;
  int free_count;
};

static TrackState g_track = {0, 0};

static void *track_alloc(size_t size) {
  g_track.alloc_count++;
  return malloc(size);
}

static void track_free(void *ptr) {
  g_track.free_count++;
  free(ptr);
}

/* A class_t with NULL move/clone/dispose — only valid for new/free */
static class_t no_callback_class = {
    .name = "no_callback",
    .size = sizeof(int),
    .move_fn = nullptr,
    .clone_fn = nullptr,
    .dispose_fn = nullptr,
};

/* Sample class_t for int objects (with default_move/default_clone) */
static class_t int_class = {
    .name = "int",
    .size = sizeof(int),
    .move_fn = default_move,
    .clone_fn = default_clone,
    .dispose_fn = nullptr,
};

/* byte class for variable-length buffers (e.g. strings) */
static class_t byte_class = {
    .name = "byte",
    .size = 1,
    .move_fn = default_move,
    .clone_fn = default_clone,
    .dispose_fn = nullptr,
};

/* ---- Dispose tracking ---- */

static int g_dispose_call_count = 0;

static void counting_dispose(void *self, allocator_t *allocator) {
  (void)self;
  (void)allocator;
  g_dispose_call_count++;
}

static class_t dispose_class = {
    .name = "dispose_int",
    .size = sizeof(int),
    .move_fn = default_move,
    .clone_fn = default_clone,
    .dispose_fn = counting_dispose,
};

/* ---- Custom move/clone for string pointer ---- */

struct string_box {
  char *str;
};

static void string_move(void *self, allocator_t *allocator, void *another) {
  (void)allocator;
  auto *dst = static_cast<string_box *>(self);
  auto *src = static_cast<string_box *>(another);
  dst->str = src->str;
  src->str = nullptr;
}

static void string_clone(void *self, allocator_t *allocator, void *another) {
  auto *dst = static_cast<string_box *>(self);
  auto *src = static_cast<string_box *>(another);
  if (src->str) {
    size_t len = strlen(src->str) + 1;
    dst->str = static_cast<char *>(allocator_new(allocator, &byte_class, len));
    memcpy(dst->str, src->str, len);
  } else {
    dst->str = nullptr;
  }
}

static void string_dispose(void *self, allocator_t *allocator) {
  auto *box = static_cast<string_box *>(self);
  if (box->str) {
    void *p = box->str;
    allocator_free(allocator, &p);
    box->str = nullptr;
  }
}

static class_t string_class = {
    .name = "string_box",
    .size = sizeof(string_box),
    .move_fn = string_move,
    .clone_fn = string_clone,
    .dispose_fn = string_dispose,
};

/* ========================================================================= */
/* Test suites                                                               */
/* ========================================================================= */

/* ---- CreateDelete ---- */

TEST(CreateDelete, ValidAllocator) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  ASSERT_NE(a, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
  EXPECT_EQ(a, nullptr);
}

TEST(CreateDelete, NullAllocFn) {
  EXPECT_EQ(create_allocator(nullptr, test_free), nullptr);
}

TEST(CreateDelete, NullFreeFn) {
  EXPECT_EQ(create_allocator(test_alloc, nullptr), nullptr);
}

TEST(CreateDelete, BothNullFn) {
  EXPECT_EQ(create_allocator(nullptr, nullptr), nullptr);
}

TEST(CreateDelete, NullDoublePtr) {
  delete_allocator(nullptr); // no crash
}

TEST(CreateDelete, NullPtr) {
  allocator_t *p = nullptr;
  EXPECT_ALLOCATOR_EMPTY_DELETE(&p); // no crash
  EXPECT_EQ(p, nullptr);
}

TEST(CreateDelete, DeleteAndNullify) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  ASSERT_NE(a, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
  EXPECT_EQ(a, nullptr);
}

/* ---- AllocatorNew ---- */

TEST(AllocatorNew, SingleInt) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data = allocator_new(a, &int_class, 1);
  ASSERT_NE(data, nullptr);

  int *p = static_cast<int *>(data);
  EXPECT_EQ(*p, 0); // zero-initialized
  *p = 42;
  EXPECT_EQ(*p, 42);

  allocator_free(a, &data);
  EXPECT_EQ(data, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNew, MultipleInts) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  constexpr size_t count = 5;
  void *data = allocator_new(a, &int_class, count);
  ASSERT_NE(data, nullptr);

  int *p = static_cast<int *>(data);
  for (size_t i = 0; i < count; i++) {
    EXPECT_EQ(p[i], 0);
    p[i] = static_cast<int>(i * 10);
  }
  for (size_t i = 0; i < count; i++) {
    EXPECT_EQ(p[i], static_cast<int>(i * 10));
  }

  allocator_free(a, &data);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNew, ZeroCount) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(allocator_new(a, &int_class, 0), nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNew, NullAllocator) {
  EXPECT_EQ(allocator_new(nullptr, &int_class, 1), nullptr);
}

TEST(AllocatorNew, NullClass) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(allocator_new(a, nullptr, 1), nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNew, ClassZeroSize) {
  class_t zero_class = {
      .name = "zero",
      .size = 0,
      .move_fn = nullptr,
      .clone_fn = nullptr,
      .dispose_fn = nullptr,
  };
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(allocator_new(a, &zero_class, 1), nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNew, GetClassReturnsCorrect) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data = allocator_new(a, &int_class, 1);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(allocator_get_class(data), &int_class);
  allocator_free(a, &data);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNew, GetCountReturnsCorrect) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data = allocator_new(a, &int_class, 3);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(allocator_get_count(data), 3u);
  allocator_free(a, &data);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNew, GetClassNullData) {
  EXPECT_EQ(allocator_get_class(nullptr), nullptr);
}

TEST(AllocatorNew, GetCountNullData) {
  EXPECT_EQ(allocator_get_count(nullptr), 0u);
}

/* ---- AllocatorNewEx ---- */

TEST(AllocatorNewEx, Basic) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data = allocator_new_ex(
      a, "point", sizeof(double), default_move, default_clone, nullptr, 1);
  ASSERT_NE(data, nullptr);

  double *p = static_cast<double *>(data);
  EXPECT_DOUBLE_EQ(*p, 0.0);
  *p = 3.14;
  EXPECT_DOUBLE_EQ(*p, 3.14);

  allocator_free(a, &data);
  EXPECT_EQ(data, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNewEx, GetClassInfo) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data = allocator_new_ex(
      a, "point", sizeof(double), default_move, default_clone, nullptr, 2);
  ASSERT_NE(data, nullptr);

  const class_t *clazz = allocator_get_class(data);
  ASSERT_NE(clazz, nullptr);
  EXPECT_STREQ(clazz->name, "point");
  EXPECT_EQ(clazz->size, sizeof(double));
  EXPECT_EQ(clazz->move_fn, default_move);
  EXPECT_EQ(clazz->clone_fn, default_clone);
  EXPECT_EQ(clazz->dispose_fn, nullptr);
  EXPECT_EQ(allocator_get_count(data), 2u);

  allocator_free(a, &data);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNewEx, NullMoveCloneDispose) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data =
      allocator_new_ex(a, "raw", sizeof(int), nullptr, nullptr, nullptr, 1);
  ASSERT_NE(data, nullptr);

  const class_t *clazz = allocator_get_class(data);
  ASSERT_NE(clazz, nullptr);
  EXPECT_EQ(clazz->move_fn, nullptr);
  EXPECT_EQ(clazz->clone_fn, nullptr);
  EXPECT_EQ(clazz->dispose_fn, nullptr);

  allocator_free(a, &data);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNewEx, ZeroSize) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(allocator_new_ex(a, "zero", 0, nullptr, nullptr, nullptr, 1),
            nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNewEx, ZeroCount) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(
      allocator_new_ex(a, "zero", sizeof(int), nullptr, nullptr, nullptr, 0),
      nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorNewEx, NullAllocator) {
  EXPECT_EQ(allocator_new_ex(
                nullptr, "zero", sizeof(int), nullptr, nullptr, nullptr, 1),
            nullptr);
}

/* ---- AllocatorFree ---- */

TEST(AllocatorFree, FreeAndNullify) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data = allocator_new(a, &int_class, 1);
  ASSERT_NE(data, nullptr);
  allocator_free(a, &data);
  EXPECT_EQ(data, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorFree, FreeNullData) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *p = nullptr;
  allocator_free(a, &p); // no crash
  EXPECT_EQ(p, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorFree, FreeNullDoublePtr) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  allocator_free(a, nullptr); // no crash
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorFree, FreeNullAllocator) {
  int dummy = 0;
  void *p = &dummy;
  allocator_free(nullptr, &p); // no crash, pointer unchanged
}

TEST(AllocatorFree, CallsDispose) {
  g_dispose_call_count = 0;
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data = allocator_new(a, &dispose_class, 1);
  ASSERT_NE(data, nullptr);
  allocator_free(a, &data);
  EXPECT_EQ(g_dispose_call_count, 1);
  EXPECT_EQ(data, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorFree, DisposeNullSafe) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *data = allocator_new(a, &no_callback_class, 1);
  ASSERT_NE(data, nullptr);
  allocator_free(a, &data); // dispose_fn is nullptr, no crash
  EXPECT_EQ(data, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ---- AllocatorMove ---- */

TEST(AllocatorMove, BasicWithDefaultMove) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *src = allocator_new(a, &int_class, 1);
  ASSERT_NE(src, nullptr);
  *static_cast<int *>(src) = 42;

  void *dst = allocator_move(a, &src);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(*static_cast<int *>(dst), 42);
  EXPECT_EQ(src, nullptr);

  allocator_free(a, &dst);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorMove, CustomMoveFn) {
  allocator_t *a = create_allocator(test_alloc, test_free);

  void *src = allocator_new(a, &string_class, 1);
  ASSERT_NE(src, nullptr);
  auto *src_box = static_cast<string_box *>(src);
  const char *hello = "hello";
  size_t len = strlen(hello) + 1;
  src_box->str = static_cast<char *>(allocator_new(a, &byte_class, len));
  memcpy(src_box->str, hello, len);

  char *saved_str = src_box->str;

  void *dst = allocator_move(a, &src);
  ASSERT_NE(dst, nullptr);
  auto *dst_box = static_cast<string_box *>(dst);
  EXPECT_EQ(dst_box->str, saved_str); // same pointer — ownership transferred
  EXPECT_STREQ(dst_box->str, "hello");
  EXPECT_EQ(src, nullptr); // source pointer nullified

  allocator_free(a, &dst);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorMove, NullObject) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *p = nullptr;
  EXPECT_EQ(allocator_move(a, &p), nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorMove, NullPtr) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(allocator_move(a, nullptr), nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorMove, NullAllocator) {
  void *p = nullptr;
  EXPECT_EQ(allocator_move(nullptr, &p), nullptr);
}

/* ---- AllocatorClone ---- */

TEST(AllocatorClone, BasicWithDefaultClone) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *src = allocator_new(a, &int_class, 1);
  ASSERT_NE(src, nullptr);
  *static_cast<int *>(src) = 42;

  void *dst = allocator_clone(a, &src);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(*static_cast<int *>(dst), 42);
  EXPECT_EQ(*static_cast<int *>(src), 42); // source unchanged

  *static_cast<int *>(dst) = 100;
  EXPECT_EQ(*static_cast<int *>(src), 42);

  allocator_free(a, &src);
  allocator_free(a, &dst);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorClone, CustomCloneFn) {
  allocator_t *a = create_allocator(test_alloc, test_free);

  void *src = allocator_new(a, &string_class, 1);
  ASSERT_NE(src, nullptr);
  auto *src_box = static_cast<string_box *>(src);
  const char *world = "world";
  size_t len = strlen(world) + 1;
  src_box->str = static_cast<char *>(allocator_new(a, &byte_class, len));
  memcpy(src_box->str, world, len);

  void *dst = allocator_clone(a, &src);
  ASSERT_NE(dst, nullptr);
  auto *dst_box = static_cast<string_box *>(dst);
  EXPECT_STREQ(dst_box->str, "world");
  EXPECT_STREQ(src_box->str, "world"); // source unchanged

  dst_box->str[0] = 'W';
  EXPECT_STREQ(src_box->str, "world"); // source unaffected

  allocator_free(a, &src);
  allocator_free(a, &dst);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorClone, NullObject) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *p = nullptr;
  EXPECT_EQ(allocator_clone(a, &p), nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorClone, NullPtr) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(allocator_clone(a, nullptr), nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(AllocatorClone, NullAllocator) {
  void *p = nullptr;
  EXPECT_EQ(allocator_clone(nullptr, &p), nullptr);
}

/* ---- DefaultCallbacks ---- */

TEST(DefaultCallbacks, DefaultMoveViaAllocatorMove) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *src = allocator_new(a, &int_class, 2);
  ASSERT_NE(src, nullptr);
  int *src_ints = static_cast<int *>(src);
  src_ints[0] = 10;
  src_ints[1] = 20;

  void *dst = allocator_move(a, &src);
  ASSERT_NE(dst, nullptr);
  int *dst_ints = static_cast<int *>(dst);
  EXPECT_EQ(dst_ints[0], 10);
  EXPECT_EQ(dst_ints[1], 20);
  EXPECT_EQ(src, nullptr);

  allocator_free(a, &dst);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(DefaultCallbacks, DefaultCloneViaAllocatorClone) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *src = allocator_new(a, &int_class, 2);
  ASSERT_NE(src, nullptr);
  int *src_ints = static_cast<int *>(src);
  src_ints[0] = 30;
  src_ints[1] = 40;

  void *dst = allocator_clone(a, &src);
  ASSERT_NE(dst, nullptr);
  int *dst_ints = static_cast<int *>(dst);
  EXPECT_EQ(dst_ints[0], 30);
  EXPECT_EQ(dst_ints[1], 40);
  EXPECT_EQ(src_ints[0], 30);
  EXPECT_EQ(src_ints[1], 40);

  allocator_free(a, &src);
  allocator_free(a, &dst);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ---- TrackingAllocator ---- */

TEST(TrackingAllocator, CountsMatch) {
  g_track = {0, 0};
  allocator_t *a = create_allocator(track_alloc, track_free);

  void *data = allocator_new(a, &int_class, 1);
  EXPECT_EQ(g_track.alloc_count, 1);
  EXPECT_EQ(g_track.free_count, 0);

  allocator_free(a, &data);
  EXPECT_EQ(g_track.free_count, 1);
  EXPECT_EQ(data, nullptr);

  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ---- OwnershipChain (integration) ---- */

TEST(OwnershipChain, AllocateMoveFree) {
  allocator_t *a = create_allocator(test_alloc, test_free);

  void *obj = allocator_new(a, &int_class, 1);
  ASSERT_NE(obj, nullptr);
  *static_cast<int *>(obj) = 123;

  void *moved = allocator_move(a, &obj);
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(obj, nullptr);
  EXPECT_EQ(*static_cast<int *>(moved), 123);

  allocator_free(a, &moved);
  EXPECT_EQ(moved, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(OwnershipChain, AllocateCloneFreeBoth) {
  allocator_t *a = create_allocator(test_alloc, test_free);

  void *obj = allocator_new(a, &int_class, 1);
  ASSERT_NE(obj, nullptr);
  *static_cast<int *>(obj) = 456;

  void *cloned = allocator_clone(a, &obj);
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(*static_cast<int *>(obj), 456);
  EXPECT_EQ(*static_cast<int *>(cloned), 456);

  *static_cast<int *>(obj) = 789;
  EXPECT_EQ(*static_cast<int *>(cloned), 456); // clone unaffected

  allocator_free(a, &obj);
  allocator_free(a, &cloned);
  EXPECT_EQ(obj, nullptr);
  EXPECT_EQ(cloned, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(OwnershipChain, NestedAllocation) {
  allocator_t *a = create_allocator(test_alloc, test_free);

  void *box = allocator_new(a, &string_class, 1);
  ASSERT_NE(box, nullptr);
  auto *b = static_cast<string_box *>(box);
  const char *msg = "nested";
  size_t len = strlen(msg) + 1;
  b->str = static_cast<char *>(allocator_new(a, &byte_class, len));
  memcpy(b->str, msg, len);

  EXPECT_STREQ(b->str, "nested");

  allocator_free(a, &box);
  EXPECT_EQ(box, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Leak Detection ==== */

TEST(LeakDetection, NoLeakCleanDelete) {
  /* All objects freed before delete_allocator — no leak output */
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *p1 = allocator_new(a, &int_class, 1);
  void *p2 = allocator_new(a, &int_class, 1);
  allocator_free(a, &p1);
  allocator_free(a, &p2);
  /* delete_allocator should not print any leak warnings */
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
  EXPECT_EQ(a, nullptr);
}

TEST(LeakDetection, LeakReportedOnDelete) {
  /* Intentionally leak an allocation and capture stderr */
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *leaked = allocator_new(a, &int_class, 3);

  /* Redirect stderr to a temp file to capture leak output */
  char tmp_path[L_tmpnam];
  tmpnam(tmp_path);
  FILE *tmp = freopen(tmp_path, "w", stderr);
  ASSERT_NE(tmp, nullptr);

  delete_allocator(&a);

  fflush(stderr);
  freopen("CON", "w", stderr); /* restore stderr on Windows */
  EXPECT_EQ(a, nullptr);

  /* Read back the captured output */
  FILE *rf = fopen(tmp_path, "r");
  ASSERT_NE(rf, nullptr);
  char buf[1024] = {};
  size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
  fclose(rf);
  remove(tmp_path);
  buf[n] = '\0';

  EXPECT_NE(std::string(buf).find("memory leak detected"), std::string::npos);
  EXPECT_NE(std::string(buf).find("int"), std::string::npos);
  EXPECT_NE(std::string(buf).find("count=3"), std::string::npos);

  /* Clean up the leaked memory manually — not via allocator_free (it's gone) */
  /* We saved `leaked` which points to user data; header is just before it */
  /* The header+data was allocated by test_alloc (= malloc), so free the raw ptr */
  free((char *)leaked - sizeof(void *) * 5 /* approx header with prev/next */);
}

TEST(LeakDetection, MultipleLeaksReported) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *p1 = allocator_new(a, &int_class, 1);
  void *p2 = allocator_new(a, &byte_class, 10);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);

  char tmp_path[L_tmpnam];
  tmpnam(tmp_path);
  FILE *tmp = freopen(tmp_path, "w", stderr);
  ASSERT_NE(tmp, nullptr);

  delete_allocator(&a);

  fflush(stderr);
  freopen("CON", "w", stderr);

  FILE *rf = fopen(tmp_path, "r");
  ASSERT_NE(rf, nullptr);
  char buf[2048] = {};
  size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
  fclose(rf);
  remove(tmp_path);
  buf[n] = '\0';

  std::string output(buf);
  EXPECT_NE(output.find("int"), std::string::npos);
  EXPECT_NE(output.find("byte"), std::string::npos);

  /* Free the deliberately-leaked allocations. delete_allocator has already
   * released the allocator struct, so reclaim the raw blocks directly.
   * HDR must match the layout of the internal alloc_header_t (5 pointers). */
  enum { HDR = sizeof(void *) * 5 };
  free((char *)p1 - HDR);
  free((char *)p2 - HDR);
}

/* ==== Memory Budget Guard ==== */

static void alloc_panic_throw(const char *msg) {
  (void)msg;
  abort();
}

TEST(MemoryBudget, TotalBytesTracked) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  void *p1 = allocator_new(a, &int_class, 1);
  ASSERT_NE(p1, nullptr);
  size_t after_one = allocator_total_bytes(a);
  EXPECT_GT(after_one, 0u);

  void *p2 = allocator_new(a, &byte_class, 100);
  ASSERT_NE(p2, nullptr);
  size_t after_two = allocator_total_bytes(a);
  EXPECT_GT(after_two, after_one);

  allocator_free(a, &p1);
  EXPECT_LT(allocator_total_bytes(a), after_two);

  allocator_free(a, &p2);
  EXPECT_EQ(allocator_total_bytes(a), 0u);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(MemoryBudget, SetMaxBytesEnforced) {
  panic_handler_t saved = get_panic_handler();
  set_panic_handler(alloc_panic_throw);

  allocator_t *a = create_allocator(test_alloc, test_free);
  /* Set a tiny limit — one allocation fits, the second must panic.
   * Internal header is ~40 bytes on 64-bit; one int alloc ≈ 44 bytes,
   * two would be ≈ 88 bytes. 64 bytes allows one but not two. */
  allocator_set_max_bytes(a, 64);

  void *p = allocator_new(a, &int_class, 1);
  ASSERT_NE(p, nullptr);

  /* Second allocation should exceed the budget and panic. */
  EXPECT_DEATH(allocator_new(a, &int_class, 1), ".*");

  allocator_free(a, &p);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
  set_panic_handler(saved);
}

TEST(MemoryBudget, ZeroMaxBytesDisablesGuard) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  allocator_set_max_bytes(a, 0); /* unlimited */

  /* Should not panic even though many allocations happen. */
  for (int i = 0; i < 1000; i++) {
    void *p = allocator_new(a, &int_class, 1);
    ASSERT_NE(p, nullptr);
    allocator_free(a, &p);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}
