#include <gtest/gtest.h>
#include "core/error.h"
#include "core/allocator.h"
#include "core/location.h"
#include "core/string.h"
#include <stdlib.h>
#include <string.h>
#include <string>

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

TEST(ErrorDiagnosticTest, RendersSingleLineWithCaret) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "var x = ;\n";
  location_t loc = make_loc(src, 8, 9);
  clux_error_t *err = create_error(a, &loc, "unexpected token");
  error_set_source(err, src);
  EXPECT_STREQ(error_get_source(err), src);

  string_t *out = error_to_string(err, a);
  std::string expected = "error: unexpected token\n"
                         " --> test.clux:1:9\n"
                         "  |\n"
                         "1 | var x = ;\n"
                         "  | " +
                         std::string(8, ' ') + "^\n";
  EXPECT_STREQ(string_get_data(out), expected.c_str());

  delete_string(&out);
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorDiagnosticTest, RendersSecondLineAndSpan) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "line one\nline two\n";
  location_t loc = make_loc(src, 14, 17);
  clux_error_t *err = create_error(a, &loc, "bad name");
  error_set_source(err, src);

  string_t *out = error_to_string(err, a);
  std::string expected = "error: bad name\n"
                         " --> test.clux:2:6\n"
                         "  |\n"
                         "2 | line two\n"
                         "  | " +
                         std::string(5, ' ') + "^^^\n";
  EXPECT_STREQ(string_get_data(out), expected.c_str());

  delete_string(&out);
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorDiagnosticTest, ExpandsTabsInSourceAndCaret) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\tvar x = ;\n";
  location_t loc = make_loc(src, 9, 10);
  clux_error_t *err = create_error(a, &loc, "syntax");
  error_set_source(err, src);

  string_t *out = error_to_string(err, a);
  std::string expected = "error: syntax\n"
                         " --> test.clux:1:13\n"
                         "  |\n"
                         "1 |     var x = ;\n"
                         "  | " +
                         std::string(12, ' ') + "^\n";
  EXPECT_STREQ(string_get_data(out), expected.c_str());

  delete_string(&out);
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorDiagnosticTest, MultiLineSpanStopsAtLineEnd) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc\ndef\n";
  location_t loc = make_loc(src, 1, 5);
  loc.end.line = 2;
  clux_error_t *err = create_error(a, &loc, "span");
  error_set_source(err, src);

  string_t *out = error_to_string(err, a);
  std::string expected = "error: span\n"
                         " --> test.clux:1:2\n"
                         "  |\n"
                         "1 | abc\n"
                         "  |  ^^\n";
  EXPECT_STREQ(string_get_data(out), expected.c_str());

  delete_string(&out);
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorDiagnosticTest, WithoutSourcePrintsHeaderOnly) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  location_t loc = make_loc(src, 0, 1);
  loc.begin.line = 0;
  loc.begin.column = 0;
  clux_error_t *err = create_error(a, &loc, "no source");

  string_t *out = error_to_string(err, a);
  std::string expected = "error: no source\n --> test.clux:1:1\n";
  EXPECT_STREQ(string_get_data(out), expected.c_str());

  delete_string(&out);
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorDiagnosticTest, MissingFilenameUsesPlaceholder) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  location_t loc = make_loc(src, 0, 1);
  loc.filename = nullptr;
  clux_error_t *err = create_error(a, &loc, "no file");
  error_set_source(err, src);

  string_t *out = error_to_string(err, a);
  EXPECT_NE(strstr(string_get_data(out), " --> <unknown>:1:1\n"), nullptr);

  delete_string(&out);
  delete_error(&err);
  delete_allocator(&a);
}

TEST(ErrorDiagnosticTest, NoLeak) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  const char *src = "abc\n";
  location_t loc = make_loc(src, 0, 1);
  clux_error_t *err = create_error(a, &loc, "leak check");
  error_set_source(err, src);
  string_t *out = error_to_string(err, a);
  delete_string(&out);
  delete_error(&err);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
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
