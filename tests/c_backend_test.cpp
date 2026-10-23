#include "test_common.h"

#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace {

std::filesystem::path default_exe(const std::string &src) {
  std::filesystem::path exe(src);
#ifdef _WIN32
  exe.replace_extension(".exe");
#else
  exe.replace_extension();
#endif
  return exe;
}

std::string cli_path() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof buf);
  std::filesystem::path self(std::string(buf, n));
  return (self.parent_path() / "clux.exe").string();
#else
  std::error_code ec;
  std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || self.empty()) return "clux";
  return (self.parent_path() / "clux").string();
#endif
}

std::string q(const std::string &s) { return "\"" + s + "\""; }

int run_build(const std::string &src, const std::string &exe,
              std::string *err) {
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  const std::string inner =
      q(cli_path()) + " build " + q(std::filesystem::absolute(src).string());
#ifdef _WIN32
  const std::string cmd = "(" + inner + ")";
#else
  const std::string cmd = inner;
#endif
  int status = std::system(cmd.c_str());
#ifdef _WIN32
  const int rc = status;
#else
  const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
  if (err) *err = slurp((std::filesystem::path(exe).parent_path() / "cbackend_err.txt").string());
  return rc;
}

int run_exe(const std::string &exe) {
  std::string cmd = q(exe);
#ifdef _WIN32
  cmd = "(" + cmd + ")";
#endif
  int status = std::system(cmd.c_str());
#ifdef _WIN32
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

} // namespace

TEST(CBackend, BuildMainReturnZero) {
  const std::string src = write_temp_file(
      "func main(): i32 { return 0; }\n", "cbackend_zero");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe(src);
  exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildMainReturnValueAndExpression) {
  const std::string src = write_temp_file(
      "func main(): i32 { return (1 + 2) * 3; }\n", "cbackend_expr");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe(src);
  exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 9);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildMainVoid) {
  const std::string src = write_temp_file(
      "func main(): void { }\n", "cbackend_void");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe(src);
  exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildControlFlowAndCalls) {
  const std::string src = write_temp_file(
      "func inc(x: i32): i32 { return x + 1; }\n"
      "func main(): i32 {\n"
      "  var x = 1;\n"
      "  var i = 0;\n"
      "  while (i < 3) {\n"
      "    x = inc(x);\n"
      "    i += 1;\n"
      "  }\n"
      "  if (x == 4) {\n"
      "    return 0;\n"
      "  } else {\n"
      "    return 1;\n"
      "  }\n"
      "}\n",
      "cbackend_m1");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildForGlobalAndDoWhile) {
  const std::string src = write_temp_file(
      "var counter: i32 = 1;\n"
      "func bump(): i32 { return counter + 1; }\n"
      "func main(): i32 {\n"
      "  var sum = 0;\n"
      "  for (var i = 0; i < 4; i = i + 1) { sum = sum + i; }\n"
      "  if (sum != 6) { return 1; }\n"
      "  if (bump() != 2) { return 2; }\n"
      "  var n = 0;\n"
      "  do { n += 1; } while (n < 3);\n"
      "  return n;\n"
      "}\n",
      "cbackend_m2");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 3);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildStructArrayPointer) {
  const std::string src = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func sum(p: Point): i32 { return p.x + p.y; }\n"
      "func main(): i32 {\n"
      "  var p: Point = .Point{ .x = 2, .y = 3 };\n"
      "  if (p.x != 2 || p.y != 3) { return 1; }\n"
      "  p.x = 5;\n"
      "  var r: ref *Point = p.&;\n"
      "  r.y = 7;\n"
      "  if (sum(p) != 12) { return 2; }\n"
      "  var a: [3]i32 = .[3]i32{ 1, 2, 3 };\n"
      "  a[1] += 4;\n"
      "  if (a[1] != 6) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_m3");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, ArrayDoesNotDecay) {
  const std::string src = write_temp_file(
      "func mutate(a: [2]i32): i32 { a[0] = 9; return a[0]; }\n"
      "func main(): i32 {\n"
      "  var a: [2]i32 = .[2]i32{ 1, 2 };\n"
      "  if (mutate(a) != 9) { return 1; }\n"
      "  if (a[0] != 1) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_array_value");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildDeferAndOwn) {
  const std::string src = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "var g: i32 = 0;\n"
      "func f(): i32 {\n"
      "  defer { g = g * 10 + 1; }\n"
      "  defer { g = g * 10 + 2; }\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 0) { return 1; }\n"
      "  if (g != 21) { return 2; }\n"
      "  var p: own *Point = new Point{ .x = 4, .y = 5 };\n"
      "  if (p.x != 4) { return 3; }\n"
      "  return p.y;\n"
      "}\n",
      "cbackend_m4");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 5);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildOwnCompositeAndArray) {
  const std::string src = write_temp_file(
      "struct Box { p: own *i32; }\n"
      "func main(): i32 {\n"
      "  var b: own *Box = new Box{ .p = new i32{7} };\n"
      "  if (b.p.* != 7) { return 1; }\n"
      "  var a: own [2]own *i32 = .[2]own *i32{ new i32{1}, new i32{2} };\n"
      "  if (a[0].* != 1) { return 2; }\n"
      "  if (a[1].* != 2) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_m4_own");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, RejectsMainWithParameters) {
  const std::string src = write_temp_file(
      "func main(x: i32): i32 { return x; }\n", "cbackend_param");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe(src);
  exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 1);
}
