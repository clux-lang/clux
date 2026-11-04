#include <gtest/gtest.h>
#include "core/error.h"
#include "core/allocator.h"
#include "core/location.h"
#include <string.h>

static location_t make_loc(const char *src, size_t begin_off, size_t end_off) {
  location_t loc;
  loc.filename = "test.clux";
  loc.begin.offset = src + begin_off;
  loc.begin.line = 10;
  loc.begin.column = (uint32_t)(begin_off + 1);
  loc.end.offset = src + end_off;
  loc.end.line = 10;
  loc.end.column = (uint32_t)(end_off + 1);
  return loc;
}

TEST(ErrorTest, CreateAndGetMessage) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "let x = ;";
  location_t loc = make_loc(src, 0, 3);
  clux_error_t *err = create_error(a, &loc, "syntax error at line %u", 1u);
  EXPECT_NE(err, nullptr);
  EXPECT_STREQ(error_get_message(err), "syntax error at line 1");
  delete_error(&err);
  EXPECT_EQ(err, nullptr);
  delete_allocator(&a);
}

TEST(ErrorTest, CreateWithPlainMessage) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "let x = ;";
  location_t loc = make_loc(src, 0, 3);
  clux_error_t *err = create_error(a, &loc, "unexpected token");
  EXPECT_STREQ(error_get_message(err), "unexpected token");
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorTest, CreateWithEmptyMessage) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "let x = ;";
  location_t loc = make_loc(src, 0, 3);
  clux_error_t *err = create_error(a, &loc, "");
  EXPECT_STREQ(error_get_message(err), "");
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorTest, CreateWithLongMessage) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "let x = ;";
  location_t loc = make_loc(src, 0, 3);
  // 构造长消息验证 vsnprintf 两遍调用正确
  clux_error_t *err = create_error(
      a, &loc, "error: %s failed at %s:%u", "parse", "file.clux", 42u);
  EXPECT_STREQ(error_get_message(err), "error: parse failed at file.clux:42");
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorTest, GetLocationReturnsBorrowedPointer) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "let x = ;";
  location_t loc = make_loc(src, 0, 3);
  clux_error_t *err = create_error(a, &loc, "err");
  const location_t *got = error_get_location(err);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->begin.offset, src);
  EXPECT_EQ(got->end.offset, src + 3);
  EXPECT_EQ(got->begin.line, 10u);
  EXPECT_EQ(got->begin.column, 1u);
  // filename 借用
  EXPECT_STREQ(got->filename, "test.clux");
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorTest, DeleteNullIsNoop) {
  clux_error_t *err = nullptr;
  delete_error(&err);
  EXPECT_EQ(err, nullptr);
  delete_error(nullptr);
}

TEST(ErrorTest, CreateMultipleErrors) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "let x = ;";
  location_t loc1 = make_loc(src, 0, 3);
  location_t loc2 = make_loc(src, 4, 5);
  clux_error_t *e1 = create_error(a, &loc1, "first error");
  clux_error_t *e2 = create_error(a, &loc2, "second error %d", 2);
  EXPECT_STREQ(error_get_message(e1), "first error");
  EXPECT_STREQ(error_get_message(e2), "second error 2");
  delete_error(&e1);
  delete_error(&e2);
  EXPECT_EQ(e1, nullptr);
  EXPECT_EQ(e2, nullptr);
  delete_allocator(&a);
}
