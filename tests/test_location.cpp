#include <gtest/gtest.h>
#include "core/location.h"
#include "core/allocator.h"
#include <string.h>

// 构造一个稳定的源码缓冲区，offset 指向其中
static location_t make_loc(const char *src, size_t begin_off, size_t end_off) {
  location_t loc;
  loc.filename = "test.clux";
  loc.begin.filename = "test.clux";
  loc.begin.offset = src + begin_off;
  loc.begin.line = 1;
  loc.begin.column = (uint32_t)(begin_off + 1);
  loc.end.filename = "test.clux";
  loc.end.offset = src + end_off;
  loc.end.line = 1;
  loc.end.column = (uint32_t)(end_off + 1);
  return loc;
}

TEST(LocationTest, GetExtractsSubstring) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "let x = 42;";
  location_t loc = make_loc(src, 0, 3); // "let"
  char *s = location_get(&loc, a);
  EXPECT_STREQ(s, "let");
  allocator_free(a, (void **)&s);
  delete_allocator(&a);
}

TEST(LocationTest, GetFullSource) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "hello world";
  location_t loc = make_loc(src, 0, strlen(src));
  char *s = location_get(&loc, a);
  EXPECT_STREQ(s, "hello world");
  allocator_free(a, (void **)&s);
  delete_allocator(&a);
}

TEST(LocationTest, GetEmptyRange) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  location_t loc = make_loc(src, 1, 1); // 空区间
  char *s = location_get(&loc, a);
  EXPECT_STREQ(s, "");
  allocator_free(a, (void **)&s);
  delete_allocator(&a);
}

TEST(LocationTest, GetSingleChar) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "x + y";
  location_t loc = make_loc(src, 2, 3); // "+"
  char *s = location_get(&loc, a);
  EXPECT_STREQ(s, "+");
  allocator_free(a, (void **)&s);
  delete_allocator(&a);
}

TEST(LocationTest, GetMidSlice) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "foo bar baz";
  location_t loc = make_loc(src, 4, 7); // "bar"
  char *s = location_get(&loc, a);
  EXPECT_STREQ(s, "bar");
  allocator_free(a, (void **)&s);
  delete_allocator(&a);
}

TEST(LocationTest, IsMatchesExactString) {
  const char *src = "fn main()";
  location_t loc = make_loc(src, 0, 2); // "fn"
  EXPECT_TRUE(location_is(&loc, "fn"));
}

TEST(LocationTest, IsRejectsDifferentString) {
  const char *src = "fn main()";
  location_t loc = make_loc(src, 0, 2);
  EXPECT_FALSE(location_is(&loc, "fn "));
  EXPECT_FALSE(location_is(&loc, "let"));
  EXPECT_FALSE(location_is(&loc, "f"));
}

TEST(LocationTest, IsEmptyRangeMatchesEmptyString) {
  const char *src = "abc";
  location_t loc = make_loc(src, 1, 1);
  EXPECT_TRUE(location_is(&loc, ""));
}

TEST(LocationTest, IsEmptyRangeRejectsNonEmpty) {
  const char *src = "abc";
  location_t loc = make_loc(src, 1, 1);
  EXPECT_FALSE(location_is(&loc, "x"));
}
