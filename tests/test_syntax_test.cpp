#include "test_common.h"

#include <gtest/gtest.h>
#include <string>

static int run_test_source(const std::string &src, const std::string &tag,
                           std::string *out = nullptr) {
  const std::string path = write_temp_file(src, tag);
  if (path.empty()) return -1;
  std::string captured;
  testing::internal::CaptureStdout();
  int rc = driver_test_file(path.c_str());
  captured = testing::internal::GetCapturedStdout();
  if (out) *out = captured;
  return rc;
}

TEST(TestSyntax, PassBlocks) {
  std::string out;
  int rc = run_test_source(
      "func add(a: i32, b: i32): i32 { return a + b; }\n"
      "test \"Math.Add\" { assert(add(1, 2) == 3); }\n"
      "test \"Math.More\" { assert(add(2, 3) == 5); }\n",
      "test_pass", &out);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("[  PASSED  ] 2 tests."), std::string::npos) << out;
}

TEST(TestSyntax, AssertFailureContinues) {
  std::string out;
  int rc = run_test_source(
      "test \"Suite.Fail\" { assert(1 == 2); }\n"
      "test \"Suite.Pass\" { assert(2 == 2); }\n",
      "test_fail_continue", &out);
  EXPECT_EQ(rc, 1) << out;
  EXPECT_NE(out.find("[  FAILED  ] Suite.Fail"), std::string::npos) << out;
  EXPECT_NE(out.find("[       OK ] Suite.Pass"), std::string::npos) << out;
  EXPECT_NE(out.find("assert failed: 1 == 2"), std::string::npos) << out;
}

TEST(TestSyntax, HelperAssertAndDeferCleanup) {
  std::string out;
  int rc = run_test_source(
      "var cleaned: i32 = 0;\n"
      "func check(x: i32): void {\n"
      "  defer { cleaned = cleaned + 1; }\n"
      "  assert(x == 1);\n"
      "}\n"
      "test \"Suite.HelperFail\" { check(2); }\n"
      "test \"Suite.CleanupRan\" { assert(cleaned == 1); }\n",
      "test_helper_defer", &out);
  EXPECT_EQ(rc, 1) << out;
  EXPECT_NE(out.find("[       OK ] Suite.CleanupRan"), std::string::npos) << out;
}

TEST(TestSyntax, MockFreeFunctionAndMethod) {
  std::string out;
  int rc = run_test_source(
      "func target(x: i32): i32 { return x + 1; }\n"
      "struct S {\n"
      "  x: i32;\n"
      "  func get(self: ref *S): i32 { return self.x; }\n"
      "}\n"
      "test \"Suite.Mock\" {\n"
      "  mock(\"target\", target, func(x: i32): i32 { return 7; });\n"
      "  assert(target(1) == 7);\n"
      "  var s: S = .S{ .x = 1 };\n"
      "  mock_method(S, \"get\", func(self: ref *S): i32 { return 9; });\n"
      "  assert(s.get() == 9);\n"
      "}\n"
      "test \"Suite.Restored\" {\n"
      "  assert(target(1) == 2);\n"
      "  var s: S = .S{ .x = 2 };\n"
      "  assert(s.get() == 2);\n"
      "}\n",
      "test_mock", &out);
  EXPECT_EQ(rc, 0) << out;
}

TEST(TestSyntax, MockAutoCleanupAfterFailure) {
  std::string out;
  int rc = run_test_source(
      "func target(x: i32): i32 { return x + 1; }\n"
      "test \"Suite.MockFail\" {\n"
      "  mock(\"target\", target, func(x: i32): i32 { return 7; });\n"
      "  assert(false);\n"
      "}\n"
      "test \"Suite.MockRestored\" {\n"
      "  assert(target(1) == 2);\n"
      "}\n",
      "test_mock_fail", &out);
  EXPECT_EQ(rc, 1) << out;
  EXPECT_NE(out.find("[       OK ] Suite.MockRestored"), std::string::npos)
      << out;
}

TEST(TestSyntax, MultipleSourceFiles) {
  const std::string a = write_temp_file(
      "func helper(): i32 { return 4; }\n"
      "test \"Multi.A\" { assert(helper() == 4); }\n",
      "test_multi_a");
  const std::string b = write_temp_file(
      "test \"Multi.B\" { assert(1 == 1); }\n",
      "test_multi_b");
  const char *paths[2] = {a.c_str(), b.c_str()};
  std::string captured;
  testing::internal::CaptureStdout();
  int rc = driver_test_sources(paths, 2);
  captured = testing::internal::GetCapturedStdout();
  EXPECT_EQ(rc, 0) << captured;
  EXPECT_NE(captured.find("[       OK ] Multi.A"), std::string::npos)
      << captured;
  EXPECT_NE(captured.find("[       OK ] Multi.B"), std::string::npos)
      << captured;
}

TEST(TestSyntax, NormalRunIgnoresTestBlocks) {
  const std::string path = write_temp_file(
      "func main(): i32 { return 0; }\n"
      "test \"Suite.Ignored\" { assert(no_such_symbol == 1); }\n",
      "test_ignored");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
}

TEST(TestSyntax, ExplicitUnmockAndNestedMock) {
  std::string out;
  int rc = run_test_source(
      "func target(x: i32): i32 { return x + 1; }\n"
      "test \"Suite.Unmock\" {\n"
      "  mock(\"target\", target, func(x: i32): i32 { return 3; });\n"
      "  assert(target(1) == 3);\n"
      "  unmock(\"target\");\n"
      "  assert(target(1) == 2);\n"
      "}\n"
      "test \"Suite.Nested\" {\n"
      "  mock(\"target\", target, func(x: i32): i32 { return 4; });\n"
      "  mock(\"target\", target, func(x: i32): i32 { return 5; });\n"
      "  assert(target(1) == 5);\n"
      "  unmock(\"target\");\n"
      "  assert(target(1) == 4);\n"
      "}\n",
      "test_unmock_nested", &out);
  EXPECT_EQ(rc, 0) << out;
}

TEST(TestSyntax, MockStaticAndGenericMethod) {
  std::string out;
  int rc = run_test_source(
      "struct S {\n"
      "  func origin(): i32 { return 1; }\n"
      "}\n"
      "struct Box[T] {\n"
      "  v: T;\n"
      "  func get(self: ref *Box[T]): T { return self.v; }\n"
      "}\n"
      "test \"Suite.Methods\" {\n"
      "  mock_method(S, \"origin\", func(): i32 { return 5; });\n"
      "  assert(S::origin() == 5);\n"
      "  var b: Box[i32] = .Box[i32]{ .v = 2 };\n"
      "  mock_method(typeof(b), \"get\", func(self: ref *Box[i32]): i32 { return 8; });\n"
      "  assert(b.get() == 8);\n"
      "}\n"
      "test \"Suite.MethodsRestored\" {\n"
      "  assert(S::origin() == 1);\n"
      "  var b: Box[i32] = .Box[i32]{ .v = 3 };\n"
      "  assert(b.get() == 3);\n"
      "}\n",
      "test_mock_methods", &out);
  EXPECT_EQ(rc, 0) << out;
}