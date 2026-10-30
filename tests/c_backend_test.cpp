#include "test_common.h"


#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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


/* 从生成的 C 文本里找出模块前缀 `clux_<4 位十六进制>_`。 */
std::string module_prefix_in(const std::string &text) {
  for (size_t pos = 0; (pos = text.find("clux_", pos)) != std::string::npos;
       pos += 5) {
    if (pos + 10 > text.size()) break;
    bool hex = true;
    for (size_t k = 5; k < 9; k++) {
      const char ch = text[pos + k];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) hex = false;
    }
    if (hex && text[pos + 9] == '_') return text.substr(pos, 10);
  }
  return "";
}

int run_cli(const std::vector<std::string> &args) {
  std::string inner = q(cli_path());
  for (const std::string &a : args) inner += " " + q(a);
#ifdef _WIN32
  const std::string cmd = "(" + inner + ")";
#else
  const std::string cmd = inner;
#endif
  int status = std::system(cmd.c_str());
#ifdef _WIN32
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

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

int run_exe_args(const std::string &exe, const std::string &args) {
  std::string cmd = q(exe) + " " + args;
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

TEST(CBackend, BuildMainArgvSlice) {
  const std::string src = write_temp_file(
      "func main(argv: []str): i32 {\n"
      "  if (len(argv) != 3usize) { return 10; }\n"
      "  if (argv[1] != \"hello\") { return 11; }\n"
      "  if (argv[2] != \"world\") { return 12; }\n"
      "  return 42;\n"
      "}\n",
      "cbackend_main_argv");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe_args(exe.string(), "hello world"), 42);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildMainArgcAndArgv) {
  const std::string src = write_temp_file(
      "func main(argc: i32, argv: []str): i32 {\n"
      "  return argc * 10 + (len(argv) as i32);\n"
      "}\n",
      "cbackend_main_argc_argv");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe_args(exe.string(), "a b"), 33);
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
      "  var r: *Point = p.&;\n"
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

TEST(CBackend, BuildDeferAndPointerField) {
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
      "  var pt: Point = .Point{ .x = 4, .y = 5 };\n"
      "  var p: *Point = pt.&;\n"
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

TEST(CBackend, BuildPointerCompositeAndArray) {
  const std::string src = write_temp_file(
      "struct Box { p: *i32; }\n"
      "func main(): i32 {\n"
      "  var v7: i32 = 7;\n"
      "  var bv: Box = .Box{ .p = v7.& };\n"
      "  var b: *Box = bv.&;\n"
      "  if (b.p.* != 7) { return 1; }\n"
      "  var x1: i32 = 1;\n"
      "  var x2: i32 = 2;\n"
      "  var a: [2]*i32 = .[2]*i32{ x1.&, x2.& };\n"
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

TEST(CBackend, BuildSliceMakeAndSlice) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var a: [3]i32 = .[3]i32{1, 2, 3};\n"
      "  var s: []i32 = make(a[0].&, 3);\n"
      "  if (len(s) != 3usize) { return 1; }\n"
      "  if (s[1] != 2) { return 2; }\n"
      "  var t: []i32 = s[1:3];\n"
      "  return t[0] + t[1];\n"
      "}\n",
      "cbackend_slice");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 5);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildPointerSlice) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var v5: i32 = 5;\n"
      "  var v6: i32 = 6;\n"
      "  var a: [2]*i32 = .[2]*i32{ v5.&, v6.& };\n"
      "  var s: []*i32 = make(a[0].&, 2);\n"
      "  if (len(s) != 2usize) { return 1; }\n"
      "  return s[0].* + s[1].*;\n"
      "}\n",
      "cbackend_own_slice");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 11);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildStringLiteralAndSlice) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var a: str = \"abc\";\n"
      "  var b: str = \"abc\";\n"
      "  if (a != b) { return 1; }\n"
      "  if (a == \"abd\") { return 2; }\n"
      "  if (len(a) != 3usize) { return 3; }\n"
      "  var t: []const u8 = a[1:3];\n"
      "  if (len(t) != 2usize) { return 4; }\n"
      "  if (t[0] != 'b') { return 5; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_str");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildOptionalAndUnion) {
  const std::string src = write_temp_file(
      "union Shape { radius: i32; w: i32; }\n"
      "func main(): i32 {\n"
      "  var a: ?i32 = 5;\n"
      "  if (a == nil) { return 1; }\n"
      "  if (a.! != 5) { return 2; }\n"
      "  var b: ?i32 = nil;\n"
      "  if (b != nil) { return 3; }\n"
      "  var s = .Shape{ .radius = 1 };\n"
      "  if (s is radius != true) { return 4; }\n"
      "  if (s is w != false) { return 5; }\n"
      "  if (s.radius != 1) { return 6; }\n"
      "  s.radius = 2;\n"
      "  if (s.radius != 2) { return 7; }\n"
      "  var e = .Shape{ .w = 3 };\n"
      "  if (e is w != true) { return 8; }\n"
      "  if (e.w != 3) { return 9; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_union_opt");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildUnionOwnAndOptionalField) {
  const std::string src = write_temp_file(
      "struct R { v: ?i32; }\n"
      "union U { p: *i32; n: i32; }\n"
      "func main(): i32 {\n"
      "  var r = .R{ .v = .?i32{7} };\n"
      "  if (r.v == nil) { return 1; }\n"
      "  if (r.v.! != 7) { return 2; }\n"
      "  r.v = nil;\n"
      "  if (r.v != nil) { return 3; }\n"
      "  var v5: i32 = 5;\n"
      "  var u = .U{ .p = v5.& };\n"
      "  if (u is p != true) { return 4; }\n"
      "  if (u.p.* != 5) { return 5; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_union_opt_own");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildCUnion) {
  const std::string src = write_temp_file(
      "cunion U { i: i32; n: i32; f: f32; }\n"
      "func main(): i32 {\n"
      "  var u = .U{ .i = 5 };\n"
      "  if (u.i != 5) { return 1; }\n"
      "  u.i = 7;\n"
      "  if (u.i != 7) { return 2; }\n"
      "  var v: U = u;\n"
      "  if (v.i != 7) { return 3; }\n"
      "  var w = .U{ .f = 1.5f32 };\n"
      "  if (w.f != 1.5f32) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_cunion");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildFunctionValuesAndClosure) {
  const std::string src = write_temp_file(
      "func inc(x: i32): i32 { return x + 1; }\n"
      "func apply(f: func(i32) -> i32, x: i32): i32 { return f(x); }\n"
      "func main(): i32 {\n"
      "  var g: func(i32) -> i32 = inc;\n"
      "  if (g(4) != 5) { return 1; }\n"
      "  if (apply(inc, 4) != 5) { return 2; }\n"
      "  var lit: func(i32) -> i32 = func(x: i32): i32 { return x + 2; };\n"
      "  if (lit(3) != 5) { return 3; }\n"
      "  var a = 2;\n"
      "  var cap: func |a: i32| (i32) -> i32 = func |a| (x: i32): i32 { return x + a; };\n"
      "  return cap(3);\n"
      "}\n",
      "cbackend_m5_func");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 5);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildMethods) {
  const std::string src = write_temp_file(
      "struct Point {\n"
      "  x: i32;\n"
      "  func get(self: *Point): i32 { return self.x; }\n"
      "  func plus(self: *Point, y: i32): i32 { return self.x + y; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var p = .Point{ .x = 4 };\n"
      "  if (p.get() != 4) { return 1; }\n"
      "  return p.plus(3);\n"
      "}\n",
      "cbackend_m5_method");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 7);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildStaticMethod) {
  const std::string src = write_temp_file(
      "struct Point {\n"
      "  x: i32;\n"
      "  func origin(): Point { return .Point{ .x = 3 }; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var p = Point::origin();\n"
      "  return p.x;\n"
      "}\n",
      "cbackend_m5_static_method");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 3);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildPointerCaptureClosure) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 5;\n"
      "  var p: *i32 = x.&;\n"
      "  var f: func |p: *i32| (i32) -> i32 =\n"
      "      (func |(p = (p))| (x: i32): i32 { return p.* + x; });\n"
      "  return f(1);\n"
      "}\n",
      "cbackend_m5_own_closure");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 6);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildCaptureClosureCopy) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 5;\n"
      "  var p: *i32 = x.&;\n"
      "  var f: func |p: *i32| (i32) -> i32 =\n"
      "      (func |(p = (p))| (x: i32): i32 { return p.* + x; });\n"
      "  var g: func |p: *i32| (i32) -> i32 = (f);\n"
      "  return g(1);\n"
      "}\n",
      "cbackend_m5_clone_closure");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 6);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, EmitCOnly) {
  const std::string src = write_temp_file(
      "func main(): i32 { return 7; }\n", "cbackend_emit_c");
  ASSERT_FALSE(src.empty());
  std::filesystem::path c_path(src);
  c_path.replace_extension(".c");
  std::filesystem::path h_path(src);
  h_path.replace_extension(".h");
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(c_path, ec);
  std::filesystem::remove(h_path, ec);
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--emit-c"}),
            0);
  ASSERT_TRUE(std::filesystem::exists(c_path)) << c_path.string();
  ASSERT_TRUE(std::filesystem::exists(h_path)) << h_path.string();
  const std::string text = slurp(c_path.string());
  EXPECT_NE(text.find("#include \"" + h_path.filename().string() + "\""),
            std::string::npos);
  EXPECT_NE(text.find("_fn_main"), std::string::npos);
  /* 模块符号前缀：clux_<4 位十六进制>_。 */
  EXPECT_FALSE(module_prefix_in(text).empty())
      << "no clux_<hash>_ module prefix in generated C";
  /* --emit-c 只生成 C/头文件，不调用宿主编译器。 */
  EXPECT_FALSE(std::filesystem::exists(exe));
  std::filesystem::remove(c_path);
  std::filesystem::remove(h_path);
}

TEST(CBackend, EmitCToExplicitPath) {
  const std::string src = write_temp_file(
      "func main(): i32 { return 0; }\n", "cbackend_emit_c_path");
  ASSERT_FALSE(src.empty());
  std::filesystem::path out(src);
  out.replace_extension(".generated.c");
  std::error_code ec;
  std::filesystem::remove(out, ec);
  EXPECT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--emit-c=" + std::filesystem::absolute(out).string()}),
            0);
  ASSERT_TRUE(std::filesystem::exists(out)) << out.string();
  const std::string text = slurp(out.string());
  EXPECT_NE(text.find("int main(int argc, char **argv)"), std::string::npos);
  std::filesystem::remove(out);
}

TEST(CBackend, BuildToExplicitOutput) {
  const std::string src = write_temp_file(
      "func main(): i32 { return 3; }\n", "cbackend_output");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  exe += ".custom.exe";
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--output=" + std::filesystem::absolute(exe).string()}),
            0);
  ASSERT_TRUE(std::filesystem::exists(exe)) << exe.string();
  EXPECT_EQ(run_exe(exe.string()), 3);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c", ec);
}

TEST(CBackend, BuildBitfieldBasic) {
  const std::string src = write_temp_file(
      "bitfield Flags: u32 {\n"
      "  a: 3;\n"
      "  b: 5;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (sizeof(Flags) != 4usize) { return 1; }\n"
      "  var f: Flags = .Flags{ .a = 1u32, .b = 3u32 };\n"
      "  if (f.a != 1u32 || f.b != 3u32) { return 2; }\n"
      "  f.a = 7u32;\n"
      "  f.b += 2u32;\n"
      "  if (f.a != 7u32 || f.b != 5u32) { return 3; }\n"
      "  var f2: Flags = .Flags{ .a = 0x1Fu32 };\n"
      "  if (f2.a != 7u32 || f2.b != 0u32) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_bitfield_basic");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildBitfieldMultiWordAndRuntimeValues) {
  const std::string src = write_temp_file(
      "bitfield Big: u8 {\n"
      "  a: 4;\n"
      "  b: 8;\n"
      "  c: 5;\n"
      "}\n"
      "func pass(v: u8): u8 { return v; }\n"
      "func main(): i32 {\n"
      "  if (sizeof(Big) != 3usize) { return 1; }\n"
      "  var x: Big = .Big{ .a = pass(0xFFu8), .b = pass(0xABu8),"
      " .c = pass(0x1Fu8) };\n"
      "  if (x.a != 0xFu8 || x.b != 0xABu8 || x.c != 0x1Fu8) { return 2; }\n"
      "  x.b = 0x12u8;\n"
      "  if (x.a != 0xFu8 || x.b != 0x12u8) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_bitfield_multi");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildBitfieldU64AndPointerAccess) {
  const std::string src = write_temp_file(
      "bitfield B: u64 { a: 40; b: 24; }\n"
      "func main(): i32 {\n"
      "  var x: B = .B{ .a = 0x123456789Au64, .b = 0xABCDEFu64 };\n"
      "  if (x.a != 0x123456789Au64 || x.b != 0xABCDEFu64) { return 1; }\n"
      "  var p: *B = x.&;\n"
      "  p.a = 0xFFFFFFFFFFu64;\n"
      "  if (x.a != 0xFFFFFFFFFFu64) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_bitfield_u64");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildBitfieldInStructArrayAndLoopUpdate) {
  const std::string src = write_temp_file(
      "bitfield F: u8 { a: 2; b: 6; }\n"
      "struct S { f: F; n: i32; }\n"
      "func get(s: S): u8 { return s.f.b; }\n"
      "func mk(): F { return .F{ .a = 2u8, .b = 5u8 }; }\n"
      "func main(): i32 {\n"
      "  var s: S = .S{ .f = .F{ .a = 1u8, .b = 9u8 }, .n = 3 };\n"
      "  if (s.f.b != 9u8) { return 1; }\n"
      "  s.f.b = 17u8;\n"
      "  if (s.f.b != 17u8 || s.f.a != 1u8) { return 2; }\n"
      "  if (get(s) != 17u8) { return 3; }\n"
      "  var a: [2]F = .[2]F{ mk(), .F{} };\n"
      "  if (a[0].b != 5u8) { return 4; }\n"
      "  a[1].b = 3u8;\n"
      "  if (a[1].b != 3u8) { return 5; }\n"
      "  for (var i = 0; i < 3; i += 1) { a[1].a += 1u8; }\n"
      "  if (a[1].a != 3u8) { return 6; }\n"
      "  var p: *S = s.&;\n"
      "  p.f.b = 33u8;\n"
      "  if (s.f.b != 33u8) { return 7; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_bitfield_nested");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildMetadataGlobalConstants) {
  const std::string src = write_temp_file(
      "[[metadata(\"name\", \"user\")]]\n"
      "[[metadata(\"level\", 3)]]\n"
      "struct User { }\n"
      "var n: str = get_metadata[User](\"name\");\n"
      "var lvl: i32 = get_metadata[User](\"level\");\n"
      "func main(): i32 {\n"
      "  if (n != \"user\") { return 1; }\n"
      "  if (lvl != 3) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_metadata_global");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildMetadataMembers) {
  const std::string src = write_temp_file(
      "[[metadata(\"name\", \"user\")]]\n"
      "struct User {\n"
      "  [[metadata(\"unit\", \"px\")]]\n"
      "  x: i32;\n"
      "  [[metadata(\"role\", \"handler\")]]\n"
      "  func run(self: *User): i32 { return 1; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var n: str = get_metadata[User](\"name\");\n"
      "  var u: str = get_field_metadata[User](\"x\", \"unit\");\n"
      "  var r: str = get_method_metadata[User](\"run\", \"role\");\n"
      "  if (n != \"user\" || u != \"px\" || r != \"handler\") { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_metadata_members");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildIgnoresTestBlocks) {
  const std::string src = write_temp_file(
      "func add(a: i32, b: i32): i32 { return a + b; }\n"
      "test \"Math.Add\" { assert(add(1, 2) == 3); }\n"
      "func main(): i32 { return add(2, 3); }\n",
      "cbackend_ignores_tests");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  /* 普通 build 忽略 test 块，只跑 main。 */
  EXPECT_EQ(run_exe(exe.string()), 5);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, TestRunnerPassing) {
  const std::string src = write_temp_file(
      "func add(a: i32, b: i32): i32 { return a + b; }\n"
      "test \"Math.Add\" { assert(add(1, 2) == 3); }\n"
      "test \"Math.More\" { assert(add(2, 3) == 5); }\n",
      "cbackend_tests_pass");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(run_cli({"test", std::filesystem::absolute(src).string(),
                     "--backend=c"}),
            0);
}

TEST(CBackend, TestRunnerReportsFailures) {
  const std::string src = write_temp_file(
      "test \"Suite.Fail\" { assert(1 == 2); }\n"
      "test \"Suite.Pass\" { assert(2 == 2); }\n",
      "cbackend_tests_fail");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(run_cli({"test", std::filesystem::absolute(src).string(),
                     "--backend=c"}),
            1);
}

TEST(CBackend, TestRunnerWithoutTestsFails) {
  const std::string src = write_temp_file("func main(): i32 { return 0; }\n",
                                          "cbackend_tests_none");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(run_cli({"test", std::filesystem::absolute(src).string(),
                     "--backend=c"}),
            1);
}

TEST(CBackend, BuildOptionalFieldNilConstruct) {
  const std::string src = write_temp_file(
      "struct R { v: ?i32; e: ?i32; }\n"
      "func main(): i32 {\n"
      "  var r: R = .R{ .v = .?i32{ 1 }, .e = nil };\n"
      "  if (r.e != nil) { return 1; }\n"
      "  if (r.v == nil) { return 2; }\n"
      "  return r.v.!;\n"
      "}\n",
      "cbackend_opt_field");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 1);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildCUnionEquality) {
  const std::string src = write_temp_file(
      "cunion U { i: i32; f: f32; }\n"
      "func main(): i32 {\n"
      "  var a = .U{ .i = 42 };\n"
      "  var b = .U{ .i = 42 };\n"
      "  var c = .U{ .i = 43 };\n"
      "  if (a == b != true) { return 1; }\n"
      "  if (a == c != false) { return 2; }\n"
      "  if (a != b) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_cunion_eq");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildResultSugarAndGenericStructInstance) {
  const std::string src = write_temp_file(
      "struct Result[T, E] {\n"
      "  good: bool;\n"
      "  val: ?T;\n"
      "  err: ?E;\n"
      "  func ok(self: *const Result[T, E]): bool { return self.good; }\n"
      "  func value(self: *const Result[T, E]): T { return self.val.!; }\n"
      "  func error(self: *const Result[T, E]): E { return self.err.!; }\n"
      "  func of_value(v: T): Result[T, E] {\n"
      "    return .Result[T, E]{ .good = true, .val = .?T{ v }, .err = nil };\n"
      "  }\n"
      "  func of_error(e: E): Result[T, E] {\n"
      "    return .Result[T, E]{ .good = false, .val = nil, .err = .?E{ e } };\n"
      "  }\n"
      "}\n"
      "func half(x: i32): Result[i32, str] {\n"
      "  if (x < 0) { return Result[i32, str]::of_error(\"negative\"); }\n"
      "  return Result[i32, str]::of_value(x / 2);\n"
      "}\n"
      "func use(x: i32): Result[i32, str] {\n"
      "  var v = half(x).?;\n"
      "  return Result[i32, str]::of_value(v + 1);\n"
      "}\n"
      "func main(): i32 {\n"
      "  var h = half(8);\n"
      "  if (h.value() != 4) { return 1; }\n"
      "  var e = half(0 - 1);\n"
      "  if (e.ok()) { return 2; }\n"
      "  if (e.error() != \"negative\") { return 3; }\n"
      "  var a = use(8);\n"
      "  if (!a.ok()) { return 4; }\n"
      "  if (a.value() != 5) { return 5; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_result");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildGenericStructInstanceMethods) {
  const std::string src = write_temp_file(
      "struct Box[T] {\n"
      "  v: T;\n"
      "  func get(self: *Box[T]): T { return self.v; }\n"
      "  func mk(x: T): Box[T] { return .Box[T]{ .v = x }; }\n"
      "  func echo(self: *Box[T]): T { var t: T = self.v; return t; }\n"
      "  func arr(self: *Box[T]): [2]T {\n"
      "    var a: [2]T = .[2]T{ self.v, self.v };\n"
      "    return a;\n"
      "  }\n"
      "  func copy_of(self: *Box[T]): Box[T] { return Box[T]::mk(self.v); }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var b: Box[i32] = Box[i32]::mk(21);\n"
      "  var a = b.arr();\n"
      "  if (a[0] != 21 || a[1] != 21) { return 1; }\n"
      "  var c = b.copy_of();\n"
      "  if (c.get() != 21) { return 2; }\n"
      "  return b.get() + b.echo() + c.get() - 63;\n"
      "}\n",
      "cbackend_generic_methods");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildGenericMethodInsideGenericStruct) {
  const std::string src = write_temp_file(
      "struct Box[T] {\n"
      "  v: T;\n"
      "  func size_of(self: *Box[T]): usize { return sizeof(T); }\n"
      "  func map[U](self: *Box[T], f: func(T) -> U): U { return f(self.v); }\n"
      "}\n"
      "func inc(x: i32): i32 { return x + 1; }\n"
      "func main(): i32 {\n"
      "  var b: Box[i32] = .Box[i32]{ .v = 41 };\n"
      "  if (b.size_of() != 4usize) { return 1; }\n"
      "  return b.map(inc);\n"
      "}\n",
      "cbackend_generic_method_inst");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 42);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

TEST(CBackend, BuildNestedGenericAndRvalueReceiver) {
  const std::string src = write_temp_file(
      "struct Box[T] { v: T; func get(self: *Box[T]): T { return self.v; } }\n"
      "func main(): i32 {\n"
      "  var inner: Box[i32] = .Box[i32]{ .v = 20 };\n"
      "  var outer: Box[Box[i32]] = .Box[Box[i32]]{ .v = inner };\n"
      "  /* 方法调用链：接收者是上一层调用的右值结果 */\n"
      "  return outer.get().get();\n"
      "}\n",
      "cbackend_nested_generic");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 20);
  std::filesystem::remove(exe);
  std::filesystem::remove(exe.string() + ".clux.c");
}

/* ---- M6 FFI：extern lib / extern cfunc ---- */

/* FFI 测试数据库的位置（与 clux 可执行文件同目录，CMake 产出）。 */
std::filesystem::path ffi_testdata_dir() {
  return std::filesystem::path(cli_path()).parent_path();
}

/* 链接用的导入库（Windows .lib / Linux .so / macOS .dylib）。 */
std::string ffi_testdata_link_path() {
  std::filesystem::path d = ffi_testdata_dir();
#ifdef _WIN32
  return (d / "clux_ffi_testdata.lib").string();
#elif defined(__APPLE__)
  return (d / "libclux_ffi_testdata.dylib").string();
#else
  return (d / "libclux_ffi_testdata.so").string();
#endif
}

/* 运行期动态库文件（与导入库同目录，测试时复制到 exe 旁）。 */
std::filesystem::path ffi_testdata_runtime() {
  std::filesystem::path d = ffi_testdata_dir();
#ifdef _WIN32
  return d / "clux_ffi_testdata.dll";
#elif defined(__APPLE__)
  return d / "libclux_ffi_testdata.dylib";
#else
  return d / "libclux_ffi_testdata.so";
#endif
}

void copy_ffi_runtime(const std::filesystem::path &dest_dir) {
  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);
  std::filesystem::copy_file(ffi_testdata_runtime(),
                             dest_dir / ffi_testdata_runtime().filename(),
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
}

/* extern lib "*"：符号来自当前进程（CRT），不产生链接参数。 */
TEST(CBackend, BuildExternCfuncFromProcess) {
  const std::string src = write_temp_file(
      "extern lib libc from \"*\";\n"
      "extern cfunc libc::strlen(s: str): usize;\n"
      "extern cfunc libc::abs(x: i32): i32;\n"
      "func main(): i32 {\n"
      "  if (libc::strlen(\"hello\") != 5usize) { return 1; }\n"
      "  if (libc::abs(0 - 7) != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_ffi_proc");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* extern cfunc 的 `own` 形参必须接收 move/clone 产生的将亡值：否则实参仍由
   调用方持有，而被调方按 契约释放 → double free。malloc/free 是最小的
   真实 C 符号组合（`opaque` 返回值 → `opaque` 变量）。 */
TEST(CBackend, BuildExternCfuncOwnParamRequiresMove) {
  clux_expect_ok(
      "extern lib libc from \"*\";\n"
      "extern cfunc libc::malloc(n: usize): opaque;\n"
      "extern cfunc libc::free(p: opaque): void;\n"
      "func main(): i32 {\n"
      "  var p: opaque = libc::malloc(8usize);\n"
      "  libc::free((p));\n"
      "  return 0;\n"
      "}\n",
      "ffi_own_move");
}

/* `Agg`（含 字段的按值聚合）是合法 C ABI 形态：move 把聚合整体交给
   被调方，被调方按字段释放；调用方不得再释放。 */

/* share/weak 是 RC 块内的 payload 指针：C 侧无法 retain/release，任何 C ABI
   位置（参数/返回/聚合字段/extern var/cfunc 定义）都必须拒绝。 */
TEST(CBackend, DiagnosticsPrintedOnce) {
  const std::string src = write_temp_file(
      "func main(): i32 { var x: i32 = \"s\"; return 0; }\n",
      "cbackend_diag_once");
  ASSERT_FALSE(src.empty());
  std::string err;
  const int rc = clux_run_capture(src, &err);
  EXPECT_EQ(rc, 1) << err;
  const std::string needle =
      "cannot initialize variable 'x' of type i32 with str";
  const size_t first = err.find(needle);
  ASSERT_NE(first, std::string::npos) << err;
  EXPECT_EQ(err.find(needle, first + needle.size()), std::string::npos)
      << "diagnostic printed more than once:\n" << err;
}

/* 词法错误同样只打印一次：driver_lex_into 有 diag 时写缓冲区，不再直接
   fprintf（否则会和 cleanup 的统一打印重复）。 */
TEST(CBackend, LexicalErrorPrintedOnce) {
  const std::string src = write_temp_file(
      "func main(): i32 { var s = \"abc; return 0; }\n",
      "cbackend_lex_once");
  ASSERT_FALSE(src.empty());
  std::string err;
  const int rc = clux_run_capture(src, &err);
  EXPECT_EQ(rc, 1) << err;
  const std::string needle = "unterminated string literal";
  const size_t first = err.find(needle);
  ASSERT_NE(first, std::string::npos) << err;
  EXPECT_EQ(err.find(needle, first + needle.size()), std::string::npos)
      << "lexical diagnostic printed more than once:\n" << err;
}

/* 真实动态库：struct 按值、`data()` 传数组指针、变参（struct …）、
 * `str` 返回值（strlen 求 len）。 */
TEST(CBackend, BuildExternCfuncTestdata) {
  if (!std::filesystem::exists(ffi_testdata_link_path()) ||
      !std::filesystem::exists(ffi_testdata_runtime()))
    GTEST_SKIP() << "clux_ffi_testdata library not built";
  std::string lib = ffi_testdata_link_path();
  for (char &c : lib)
    if (c == '\\') c = '/';
  const std::string src = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "extern lib td from \"" + lib + "\";\n"
      "extern cfunc td::clux_point_sum(p: Point): i32;\n"
      "extern cfunc td::clux_sum_i32(p: *i32, n: i32): i32;\n"
      "extern cfunc td::clux_point_va_sum(n: i32, ...): i32;\n"
      "extern cfunc td::clux_greeting(): str;\n"
      "func main(): i32 {\n"
      "  var p: Point = .Point{ .x = 20, .y = 22 };\n"
      "  if (td::clux_point_sum(p) != 42) { return 1; }\n"
      "  var a: [3]i32 = .[3]i32{ 1, 2, 3 };\n"
      "  if (td::clux_sum_i32(data(a), 3) != 6) { return 2; }\n"
      "  if (td::clux_point_va_sum(2, .Point{ .x = 1, .y = 2 },\n"
      "                           .Point{ .x = 20, .y = 19 }) != 42)\n"
      "    { return 3; }\n"
      "  var g: str = td::clux_greeting();\n"
      "  if (len(g) != 19usize) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_ffi_testdata");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  copy_ffi_runtime(exe.parent_path());
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* 通用库名 → `-l<name>` + 产物目录的 `-L` 搜索。 */
TEST(CBackend, BuildExternLibGenericName) {
  if (!std::filesystem::exists(ffi_testdata_link_path()) ||
      !std::filesystem::exists(ffi_testdata_runtime()))
    GTEST_SKIP() << "clux_ffi_testdata library not built";
  const std::string src = write_temp_file(
      "extern lib td from \"clux_ffi_testdata\";\n"
      "extern cfunc td::clux_greeting(): str;\n"
      "func main(): i32 {\n"
      "  var g: str = td::clux_greeting();\n"
      "  if (len(g) != 19usize) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_ffi_link");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::filesystem::path dir = exe.parent_path();
  copy_ffi_runtime(dir);
  std::error_code ec;
  /* 通用名链接需要导入库在 -L 搜索目录里（产物目录）。 */
  std::filesystem::copy_file(
      ffi_testdata_link_path(),
      dir / std::filesystem::path(ffi_testdata_link_path()).filename(),
      std::filesystem::copy_options::overwrite_existing, ec);
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* extern lib 的 `assert(<cond>)` 为假 → 前脱糖裁剪 → 不产生链接参数。 */
TEST(CBackend, BuildExternLibAssertPruned) {
  const std::string src = write_temp_file(
      "extern lib libc from \"*\";\n"
      "extern lib nope from \"definitely_missing_xyz\" "
      "assert(OS_TYPE == \"NOPE\");\n"
      "extern cfunc libc::strlen(s: str): usize;\n"
      "func main(): i32 {\n"
      "  if (libc::strlen(\"abc\") != 3usize) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_ffi_assert");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* `cfunc_ptr(命名函数)` → 生成 C trampoline；qsort 回调里改全局计数。 */
TEST(CBackend, BuildExternCfuncCallback) {
  const std::string src = write_temp_file(
      "extern lib crt from \"*\";\n"
      "extern cfunc crt::qsort(base: opaque, n: usize, size: usize,\n"
      "                        cmp: cfunc(*const opaque, *const opaque) -> i32): "
      "void;\n"
      "var CALLS: i32 = 0;\n"
      "cfunc cmp(a: *const opaque, b: *const opaque): i32 {\n"
      "  CALLS = CALLS + 1;\n"
      "  var x: i32 = (a as *const i32).*;\n"
      "  var y: i32 = (b as *const i32).*;\n"
      "  if (x < y) { return -1; }\n"
      "  if (x > y) { return 1; }\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var arr = .[5]i32{4, 2, 5, 1, 3};\n"
      "  crt::qsort(arr.&, 5, 4, cfunc_ptr(cmp));\n"
      "  if (arr[0] != 1 || arr[4] != 5) { return 1; }\n"
      "  for (var i: usize = 0usize; i + 1usize < 5usize;\n"
      "       i = i + 1usize) {\n"
      "    if (arr[i] > arr[i + 1usize]) { return 2; }\n"
      "  }\n"
      "  if (CALLS < 4) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_ffi_callback");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* 带捕获的闭包没有 C 等价物 → 明确拒绝，而不是生成错误代码。 */
TEST(CBackend, RejectsExternCfuncCapturingCallback) {
  const std::string src = write_temp_file(
      "extern lib crt from \"*\";\n"
      "extern cfunc crt::qsort(base: opaque, n: usize, size: usize,\n"
      "                        cmp: cfunc(*const opaque, *const opaque) -> i32): "
      "void;\n"
      "func main(): i32 {\n"
      "  var k: i32 = 1;\n"
      "  var cb = func |k| cbc(a: *const opaque, b: *const opaque): i32 {\n"
      "    return k;\n"
      "  };\n"
      "  var arr = .[2]i32{2, 1};\n"
      "  crt::qsort(arr.&, 2, 4, cfunc_ptr(cb));\n"
      "  return 0;\n"
      "}\n",
      "cbackend_ffi_capture");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 1);
  EXPECT_FALSE(std::filesystem::exists(exe));
}

/* 本后端 str = {char*, len}，与 C ABI 的 char* 布局不同：含 str 字段的聚合
   按值传给 C 会错位 → 声明处拒绝（而不是生成错误代码）。 */
TEST(CBackend, RejectsExternCfuncAggregateWithStr) {
  const std::string src = write_temp_file(
      "struct S { s: str; n: i32; }\n"
      "extern lib libc from \"*\";\n"
      "extern cfunc libc::take_s(x: S): i32;\n"
      "func main(): i32 { return 0; }\n",
      "cbackend_ffi_str_agg");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 1);
  EXPECT_FALSE(std::filesystem::exists(exe));
}

/* enum 在 C 侧就是底层整数：variant 是常量、变量/字段/数组/switch/cast
   全部按底层类型处理（含 u8 底层的符号/宽度归一）。 */
TEST(CBackend, BuildEnumValues) {
  const std::string src = write_temp_file(
      "enum Color: i32 { Red = 1, Green = 2, Blue = 3 }\n"
      "enum Small: u8 { A = 0u8, B = 200u8 }\n"
      "struct Paint { color: Color; shade: i32; }\n"
      "var G: Color = Color::Blue;\n"
      "func name_of(c: Color): i32 {\n"
      "  switch (c) {\n"
      "    (Color::Red) -> { return 10; }\n"
      "    (Color::Green) -> { return 20; }\n"
      "    default -> { return 30; }\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var c: Color = Color::Green;\n"
      "  if (c != Color::Green) { return 1; }\n"
      "  if (G != Color::Blue) { return 2; }\n"
      "  var p: Paint = .Paint{ .color = Color::Red, .shade = 7 };\n"
      "  if (p.color != Color::Red) { return 3; }\n"
      "  var s: Small = Small::B;\n"
      "  if (s != Small::B) { return 4; }\n"
      "  if ((c as i32) != 2) { return 5; }\n"
      "  var arr: [2]Color = .[2]Color{ Color::Red, Color::Blue };\n"
      "  if (arr[1] != Color::Blue) { return 6; }\n"
      "  if (sizeof(Color) != 4usize) { return 7; }\n"
      "  if (name_of(Color::Green) != 20) { return 8; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_enum");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* enum 作为 extern cfunc 的形参/返回：C 原型按底层整数，variant 作常量。 */
TEST(CBackend, BuildEnumExternCfunc) {
  if (!std::filesystem::exists(ffi_testdata_link_path()) ||
      !std::filesystem::exists(ffi_testdata_runtime()))
    GTEST_SKIP() << "clux_ffi_testdata library not built";
  std::string lib = ffi_testdata_link_path();
  for (char &c : lib)
    if (c == '\\') c = '/';
  const std::string src = write_temp_file(
      "enum Color: i32 { Red = 1, Green = 2 }\n"
      "extern lib td from \"" + lib + "\";\n"
      "extern cfunc td::clux_color_next(c: Color): Color;\n"
      "func main(): i32 {\n"
      "  var c: Color = td::clux_color_next(Color::Red);\n"
      "  if (c != Color::Green) { return 1; }\n"
      "  if (td::clux_color_next(Color::Green) != Color::Red) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_enum_ffi");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  copy_ffi_runtime(exe.parent_path());
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* extern var：库导出的数据符号（读/写全部标量 + struct/enum/str）。 */
TEST(CBackend, BuildExternVar) {
  if (!std::filesystem::exists(ffi_testdata_link_path()) ||
      !std::filesystem::exists(ffi_testdata_runtime()))
    GTEST_SKIP() << "clux_ffi_testdata library not built";
  std::string lib = ffi_testdata_link_path();
  for (char &c : lib)
    if (c == '\\') c = '/';
  const std::string src = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "enum Color: i32 { Red = 1, Green = 2 }\n"
      "extern lib td from \"" + lib + "\";\n"
      "extern var td::td_i32: i32;\n"
      "extern var td::td_u64: u64;\n"
      "extern var td::td_bool: bool;\n"
      "extern var td::td_f64: f64;\n"
      "extern var td::td_point: Point;\n"
      "extern var td::td_color: Color;\n"
      "extern var td::td_str: str;\n"
      "func main(): i32 {\n"
      "  if (td::td_i32 != -123456) { return 1; }\n"
      "  td::td_i32 = 42;\n"
      "  if (td::td_i32 != 42) { return 2; }\n"
      "  if (td::td_u64 != 18000000000000000000u64) { return 3; }\n"
      "  if (td::td_bool != true) { return 4; }\n"
      "  if (td::td_f64 != 2.5) { return 5; }\n"
      "  if (td::td_point.x != 3 || td::td_point.y != 4) { return 6; }\n"
      "  if (td::td_color != Color::Green) { return 7; }\n"
      "  if (len(td::td_str) != 19usize) { return 8; }\n"
      "  td::td_str = \"abc\";\n"
      "  if (len(td::td_str) != 3usize) { return 9; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_ffi_var");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  copy_ffi_runtime(exe.parent_path());
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* extern cfunc 返回 C 函数指针：拿到 cfunc 值后再传给别的 C 函数。 */
TEST(CBackend, BuildExternCfuncReturnsCallback) {
  if (!std::filesystem::exists(ffi_testdata_link_path()) ||
      !std::filesystem::exists(ffi_testdata_runtime()))
    GTEST_SKIP() << "clux_ffi_testdata library not built";
  std::string lib = ffi_testdata_link_path();
  for (char &c : lib)
    if (c == '\\') c = '/';
  const std::string src = write_temp_file(
      "extern lib td from \"" + lib + "\";\n"
      "extern cfunc td::clux_get_binop(which: i32): cfunc(i32, i32) -> i32;\n"
      "extern cfunc td::clux_apply_binop(op: cfunc(i32, i32) -> i32, a: i32,\n"
      "                                   b: i32): i32;\n"
      "cfunc neg(a: i32, b: i32): i32 { return 0 - a; }\n"
      "func main(): i32 {\n"
      "  var add: cfunc(i32, i32) -> i32 = td::clux_get_binop(0);\n"
      "  var sub: cfunc(i32, i32) -> i32 = td::clux_get_binop(1);\n"
      "  if (td::clux_apply_binop(add, 20, 22) != 42) { return 1; }\n"
      "  if (td::clux_apply_binop(sub, 50, 8) != 42) { return 2; }\n"
      "  if (td::clux_apply_binop(cfunc_ptr(neg), 5, 0) != -5) { return 3; }\n"
      "  if (td::clux_apply_binop(td::clux_get_binop(1), 50, 8) != 42)\n"
      "    { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_ffi_cretr");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  copy_ffi_runtime(exe.parent_path());
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* ---- 编译/链接拆分与工具链参数 ---- */

std::string host_cc_name() {
#ifdef _WIN32
  return "clang";
#else
  return "cc";
#endif
}

/* 默认构建保留目标文件（多文件构建可复用）：`<mirror base>.obj / .o`
 * （源文件在构建目录内时即 `<src 去扩展名>.obj`）。 */
TEST(CBackend, BuildKeepsObjectFile) {
  const std::string src =
      write_temp_file("func main(): i32 { return 9; }\n", "cbackend_obj");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  std::filesystem::path base(src);
  base.replace_extension();
#ifdef _WIN32
  std::filesystem::path obj = base.string() + ".obj";
#else
  std::filesystem::path obj = base.string() + ".o";
#endif
  EXPECT_TRUE(std::filesystem::exists(obj));
  EXPECT_EQ(run_exe(exe.string()), 9);
  std::filesystem::remove(obj, ec);
}

/* 构建目录镜像（类似 cmake build/）：`cmd/main.cx` → `build/cmd/main.c/.obj/.exe`。 */
TEST(CBackend, BuildMirrorsSourceTree) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "mirror_proj";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj / "cmd", ec);
  fs::create_directories(proj / "src" / "app", ec);
  {
    std::FILE *fp = std::fopen((proj / "cmd" / "main.cx").string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs("func main(): i32 { return 5; }\n", fp);
    std::fclose(fp);
  }
  {
    std::FILE *fp =
        std::fopen((proj / "src" / "app" / "tool.cx").string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs("func main(): i32 { return 0; }\n", fp);
    std::fclose(fp);
  }

  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(run_cli({"build", "cmd/main.cx"}), 0);
  EXPECT_EQ(run_cli({"build", "src/app/tool.cx", "--emit-c"}), 0);
  EXPECT_EQ(run_cli({"build", "cmd/main.cx", "-B", "out"}), 0);
  fs::current_path(saved);

#ifdef _WIN32
  fs::path exe = proj / "build" / "cmd" / "main.exe";
  fs::path obj = proj / "build" / "cmd" / "main.obj";
#else
  fs::path exe = proj / "build" / "cmd" / "main";
  fs::path obj = proj / "build" / "cmd" / "main.o";
#endif
  EXPECT_TRUE(fs::exists(exe)) << exe.string();
  EXPECT_TRUE(fs::exists(obj)) << obj.string();
  EXPECT_TRUE(fs::exists(proj / "build" / "src" / "app" / "tool.c"));
  EXPECT_FALSE(fs::exists(proj / "build" / "cmd" / "main.c"));
  EXPECT_EQ(run_exe(exe.string()), 5);
  EXPECT_TRUE(fs::exists(proj / "out" / "cmd" / "main.exe") ||
              fs::exists(proj / "out" / "cmd" / "main"));
  fs::remove_all(proj, ec);
}

/* `--emit-obj`：只编译到目标文件；该对象能被 host cc 独立链接成可执行文件。 */
TEST(CBackend, EmitObjectOnly) {
  const std::string src = write_temp_file(
      "func main(): i32 { return 3; }\n", "cbackend_emitobj");
  ASSERT_FALSE(src.empty());
  std::filesystem::path dir = std::filesystem::path(clux_test_temp_dir());
#ifdef _WIN32
  std::filesystem::path obj = dir / "cbackend_emitobj_manual.obj";
  std::filesystem::path exe = dir / "cbackend_emitobj_manual.exe";
#else
  std::filesystem::path obj = dir / "cbackend_emitobj_manual.o";
  std::filesystem::path exe = dir / "cbackend_emitobj_manual";
#endif
  std::error_code ec;
  std::filesystem::remove(obj, ec);
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--emit-obj=" + obj.string()}),
            0);
  ASSERT_TRUE(std::filesystem::exists(obj));
  EXPECT_FALSE(std::filesystem::exists(exe)); /* 没有链接 */
  /* 独立链接（证明目标文件是多文件构建可用的输入）。 */
  std::string link_cmd = q(host_cc_name()) + " " + q(obj.string()) + " -o " +
                         q(exe.string());
#ifdef _WIN32
  link_cmd = "(" + link_cmd + ")";
#endif
  EXPECT_EQ(std::system(link_cmd.c_str()), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 3);
  std::filesystem::remove(obj, ec);
  std::filesystem::remove(exe, ec);
}

/* `--cc/--cflag/--ldflag/-I/--include/-L/--link-lib`：工具链参数与代码声明的
 * extern lib 一起继承给 host cc；`extern lib "*"` 时不要求代码声明库。 */
TEST(CBackend, BuildWithCliToolchain) {
  if (!std::filesystem::exists(ffi_testdata_link_path()) ||
      !std::filesystem::exists(ffi_testdata_runtime()))
    GTEST_SKIP() << "clux_ffi_testdata library not built";
  const std::string src = write_temp_file(
      "extern lib td from \"*\";\n"
      "extern cfunc td::clux_greeting(): str;\n"
      "func main(): i32 {\n"
      "  var g: str = td::clux_greeting();\n"
      "  if (len(g) != 19usize) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_cli_tc");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::filesystem::path dir = exe.parent_path();
  std::filesystem::create_directories(dir);
  std::error_code ec;
  copy_ffi_runtime(dir);
  std::filesystem::copy_file(
      ffi_testdata_link_path(),
      dir / std::filesystem::path(ffi_testdata_link_path()).filename(),
      std::filesystem::copy_options::overwrite_existing, ec);
  std::filesystem::remove(exe, ec);
  const std::string tmp = std::filesystem::absolute(clux_test_temp_dir()).string();
  std::vector<std::string> args = {
      "build",
      std::filesystem::absolute(src).string(),
      "--output=" + exe.string(),
      "--cc=" + host_cc_name(),
      "--cflag=-O1",
      "--ldflag=-v",
      "-I",
      tmp,
      "--include=" + tmp,
      "-D",
      "FOO=1",
      "-L",
      dir.string(),
      "--link-lib=clux_ffi_testdata",
  };
  EXPECT_EQ(run_cli(args), 0);
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* .cx → .c/.h 配对：export 的函数/全局变量/类型进头文件，非导出 static。 */
TEST(CBackend, GeneratesHeaderPairForExports) {
  const std::string src = write_temp_file(
      "export struct Point { x: i32; y: i32; }\n"
      "export var PI: i32 = 42;\n"
      "export func add(a: i32, b: i32): i32 { return a + b + PI; }\n"
      "func helper(): i32 { return add(1, 2); }\n"
      "func main(): i32 { return helper() - 45; }\n",
      "cbackend_header");
  ASSERT_FALSE(src.empty());
  std::filesystem::path c_path(src);
  c_path.replace_extension(".c");
  std::filesystem::path h_path(src);
  h_path.replace_extension(".h");
  std::error_code ec;
  std::filesystem::remove(c_path, ec);
  std::filesystem::remove(h_path, ec);
  ASSERT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--emit-c"}),
            0);
  ASSERT_TRUE(std::filesystem::exists(h_path));
  const std::string h = slurp(h_path.string());
  const std::string c = slurp(c_path.string());

  /* 头文件：extern 函数声明 / 全局变量声明 / 导出类型定义 / guard。 */
  EXPECT_NE(h.find("extern int32_t "), std::string::npos);
  EXPECT_NE(h.find("_fn_add(void *env"), std::string::npos);
  EXPECT_NE(h.find("extern int32_t "), std::string::npos);
  EXPECT_NE(h.find("_v_PI;"), std::string::npos);
  EXPECT_NE(h.find("struct clux_"), std::string::npos);
  EXPECT_NE(h.find("int32_t clux_f_x;"), std::string::npos);
  EXPECT_NE(h.find("#ifndef CLUX_"), std::string::npos);

  /* .c：包含头文件；非导出函数是 static，导出函数不是。 */
  EXPECT_NE(c.find("#include \"" + h_path.filename().string() + "\""),
            std::string::npos);
  const size_t helper_pos = c.find("_fn_helper(void *env");
  ASSERT_NE(helper_pos, std::string::npos);
  const size_t helper_line = c.rfind('\n', helper_pos);
  const std::string helper_prefix =
      c.substr(helper_line == std::string::npos ? 0 : helper_line + 1,
               helper_pos - (helper_line == std::string::npos ? 0
                                                             : helper_line + 1));
  EXPECT_EQ(helper_prefix.find("static "), 0u)
      << "non-exported function must be static: " << helper_prefix;
  const size_t add_pos = c.find("_fn_add(void *env");
  ASSERT_NE(add_pos, std::string::npos);
  const size_t add_line = c.rfind('\n', add_pos);
  const std::string add_prefix =
      c.substr(add_line == std::string::npos ? 0 : add_line + 1,
               add_pos - (add_line == std::string::npos ? 0 : add_line + 1));
  EXPECT_EQ(add_prefix.find("static "), std::string::npos)
      << "exported function must not be static: " << add_prefix;
  /* 导出的全局变量定义不是 static。 */
  const size_t pi_pos = c.find("_v_PI = ");
  ASSERT_NE(pi_pos, std::string::npos);
  const size_t pi_line = c.rfind('\n', pi_pos);
  const std::string pi_prefix =
      c.substr(pi_line == std::string::npos ? 0 : pi_line + 1,
               pi_pos - (pi_line == std::string::npos ? 0 : pi_line + 1));
  EXPECT_EQ(pi_prefix.find("static "), std::string::npos);
  std::filesystem::remove(c_path, ec);
  std::filesystem::remove(h_path, ec);
}

/* 头文件里的类型按拓扑序（依赖先）：Inner 定义在 Outer 之前。 */
TEST(CBackend, HeaderTypesAreTopological) {
  const std::string src = write_temp_file(
      "export struct Inner { v: i32; }\n"
      "export struct Outer { i: Inner; o: ?i32; }\n"
      "func main(): i32 { return 0; }\n",
      "cbackend_topo");
  ASSERT_FALSE(src.empty());
  std::filesystem::path c_path(src);
  c_path.replace_extension(".c");
  std::filesystem::path h_path(src);
  h_path.replace_extension(".h");
  std::error_code ec;
  std::filesystem::remove(h_path, ec);
  ASSERT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--emit-c"}),
            0);
  ASSERT_TRUE(std::filesystem::exists(h_path));
  const std::string h = slurp(h_path.string());
  const size_t inner = h.find("int32_t clux_f_v;");
  const size_t outer = h.find("clux_f_i;");
  ASSERT_NE(inner, std::string::npos);
  ASSERT_NE(outer, std::string::npos);
  EXPECT_LT(inner, outer) << "Inner must be defined before Outer";
  std::filesystem::remove(c_path, ec);
  std::filesystem::remove(h_path, ec);
}

/* `export cfunc`：头文件是 C ABI 声明（无 env），.c 里是导出包装 +
 * 内部 env-first 函数（static）。 */
TEST(CBackend, ExportedCfuncUsesCApi) {
  const std::string src = write_temp_file(
      "export cfunc cadd(a: i32, b: i32): i32 { return a + b; }\n"
      "func main(): i32 { return cadd(40, 2) - 42; }\n",
      "cbackend_export_cfunc");
  ASSERT_FALSE(src.empty());
  std::filesystem::path c_path(src);
  c_path.replace_extension(".c");
  std::filesystem::path h_path(src);
  h_path.replace_extension(".h");
  std::error_code ec;
  std::filesystem::remove(h_path, ec);
  ASSERT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--emit-c"}),
            0);
  ASSERT_TRUE(std::filesystem::exists(h_path));
  const std::string h = slurp(h_path.string());
  const std::string c = slurp(c_path.string());
  EXPECT_NE(h.find("_cadd(int32_t, int32_t);"), std::string::npos);
  EXPECT_EQ(h.find("_cadd(void *env"), std::string::npos)
      << "exported cfunc header must be a plain C ABI prototype";
  /* 导出的 C ABI 包装不是 static；内部 env-first 函数是 static。 */
  const size_t w = c.find("_cadd(int32_t a0, int32_t a1) {");
  ASSERT_NE(w, std::string::npos);
  const size_t w_line = c.rfind('\n', w);
  const std::string w_prefix =
      c.substr(w_line == std::string::npos ? 0 : w_line + 1,
               w - (w_line == std::string::npos ? 0 : w_line + 1));
  EXPECT_EQ(w_prefix.find("static "), std::string::npos) << w_prefix;
  EXPECT_NE(c.find("static int32_t ") != std::string::npos, false);
  std::filesystem::remove(c_path, ec);
  std::filesystem::remove(h_path, ec);
}

/* 模块前缀来自「源文件相对项目根目录（cwd）的路径」：不同相对路径 →
 * 不同前缀，同一相对路径稳定。 */
TEST(CBackend, ModulePrefixFromRelativePath) {
  namespace fs = std::filesystem;
  fs::path root = fs::path(clux_test_temp_dir()) / "prefix_proj";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "a", ec);
  fs::create_directories(root / "b", ec);
  const char *body = "func main(): i32 { return 0; }\n";
  {
    std::FILE *fp = std::fopen((root / "a" / "mod.cx").string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(body, fp);
    std::fclose(fp);
  }
  {
    std::FILE *fp = std::fopen((root / "b" / "mod.cx").string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(body, fp);
    std::fclose(fp);
  }
  fs::path saved = fs::current_path();
  fs::current_path(root, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(run_cli({"build", "a/mod.cx", "--emit-c=a.c"}), 0);
  ASSERT_EQ(run_cli({"build", "b/mod.cx", "--emit-c=b.c"}), 0);
  fs::current_path(saved);

  auto extract = [](const std::string &text) -> std::string {
    return module_prefix_in(text);
  };
  const std::string pa = extract(slurp((root / "a.c").string()));
  const std::string pb = extract(slurp((root / "b.c").string()));
  ASSERT_FALSE(pa.empty()) << "no module prefix in a.c";
  ASSERT_FALSE(pb.empty()) << "no module prefix in b.c";
  EXPECT_NE(pa, pb);
  /* 同一相对路径稳定：重建一次前缀不变。 */
  fs::current_path(root, ec);
  ASSERT_EQ(run_cli({"build", "a/mod.cx", "--emit-c=a2.c"}), 0);
  fs::current_path(saved);
  const std::string pa2 = extract(slurp((root / "a2.c").string()));
  EXPECT_EQ(pa, pa2);
  fs::remove_all(root, ec);
}
/* 真编译单元链路：库模块（无 main）→ --emit-obj + .h；
 * 手写 C 驱动 include 头文件、调用导出的 C ABI 符号并与对象一起链接。 */
TEST(CBackend, CompileUnitLinksAcrossC) {
  namespace fs = std::filesystem;
  fs::path dir = fs::path(clux_test_temp_dir()) / "unit";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  {
    std::FILE *fp = std::fopen((dir / "lib.cx").string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs("export func add(a: i32, b: i32): i32 { return a + b; }\n"
               "func internal(): i32 { return 1; }\n",
               fp);
    std::fclose(fp);
  }
#ifdef _WIN32
  const char *obj_ext = ".obj";
#else
  const char *obj_ext = ".o";
#endif
  const std::string obj = (dir / ("lib" + std::string(obj_ext))).string();
  ASSERT_EQ(run_cli({"build", (dir / "lib.cx").string(),
                     "--emit-obj=" + obj}),
            0);
  ASSERT_TRUE(fs::exists(obj)) << "library module must compile without main";
  ASSERT_TRUE(fs::exists(dir / "lib.h"));
  const std::string h = slurp((dir / "lib.h").string());
  const size_t marker = h.find("_fn_add");
  ASSERT_NE(marker, std::string::npos) << h;
  const size_t paren = h.find('(', marker);
  ASSERT_NE(paren, std::string::npos);
  size_t begin = paren;
  while (begin > 0) {
    const char ch = h[begin - 1];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_'))
      break;
    begin--;
  }
  const std::string fname = h.substr(begin, paren - begin);
  ASSERT_FALSE(fname.empty());

  /* 手写 C 驱动：include 模块头文件，按 env-first ABI 调用导出函数。 */
  {
    std::FILE *fp = std::fopen((dir / "driver.c").string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fprintf(fp,
                 "#include \"lib.h\"\n"
                 "int main(void) { return (int)(%s(NULL, 40, 2) - 42); }\n",
                 fname.c_str());
    std::fclose(fp);
  }
  const std::string exe = (dir / ("driver" + std::string(obj_ext == std::string(".obj") ? ".exe" : ""))).string();
  std::string cmd = q(host_cc_name()) + " -I" + q(dir.string()) + " " +
                    q((dir / "driver.c").string()) + " " + q(obj) + " -o " +
                    q(exe);
#ifdef _WIN32
  cmd = "(" + cmd + ")";
  /* 宿主编译器需要可写的临时目录（clux 自己调 cc 时也会把 TMP 指向产物
     目录；测试进程里同样处理）。 */
  char old_tmp[4096] = {0}, old_temp[4096] = {0};
  size_t n_tmp = 0;
  if (getenv("TMP")) {
    snprintf(old_tmp, sizeof old_tmp, "%s", getenv("TMP"));
    n_tmp = strlen(old_tmp);
  }
  if (getenv("TEMP")) snprintf(old_temp, sizeof old_temp, "%s", getenv("TEMP"));
  _putenv_s("TMP", dir.string().c_str());
  _putenv_s("TEMP", dir.string().c_str());
#endif
  const int link_rc = std::system(cmd.c_str());
#ifdef _WIN32
  _putenv_s("TMP", n_tmp ? old_tmp : "");
  _putenv_s("TEMP", old_temp[0] ? old_temp : "");
#endif
  ASSERT_EQ(link_rc, 0) << cmd;
  ASSERT_TRUE(fs::exists(exe));
  EXPECT_EQ(run_exe(exe), 0);
  fs::remove_all(dir, ec);
}

/* import 多文件：每个模块独立生成 .c/.h/.obj，跨模块调用/变量/类型，
 * 最后统一链接（模块前缀由各自相对路径决定）。 */
TEST(CBackend, BuildImportMultiFile) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "imp_proj";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("math.cx",
        "export func add(a: i32, b: i32): i32 { return a + b; }\n"
        "export var PI: i32 = 42;\n"
        "export struct Point { x: i32; y: i32; }\n");
  write("main.cx",
        "import math from \"./math\";\n"
        "func main(): i32 {\n"
        "  var p: math::Point = .math::Point{ .x = 20, .y = 22 };\n"
        "  if (math::PI != 42) { return 1; }\n"
        "  return math::add(p.x, p.y) - math::PI;\n"
        "}\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  const int build_rc = run_cli({"build", "main.cx"});
#ifdef _WIN32
  const char *obj_ext = ".obj";
  const char *exe_name = "main.exe";
#else
  const char *obj_ext = ".o";
  const char *exe_name = "main";
#endif
  EXPECT_EQ(build_rc, 0);
  fs::current_path(saved);
  ASSERT_TRUE(fs::exists(proj / "build" / exe_name));
  EXPECT_TRUE(fs::exists(proj / "build" / ("math" + std::string(obj_ext))))
      << "dependency module must be compiled to its object";
  EXPECT_TRUE(fs::exists(proj / "build" / ("main" + std::string(obj_ext))));
  EXPECT_TRUE(fs::exists(proj / "build" / "math.h"));
  EXPECT_TRUE(fs::exists(proj / "build" / "main.h"));
  EXPECT_EQ(run_exe((proj / "build" / exe_name).string()), 0);

  /* --emit-c：所有模块的 .c/.h 都写进镜像目录，跨模块调用是 extern 原型。 */
  fs::current_path(proj, ec);
  ASSERT_EQ(run_cli({"build", "main.cx", "--emit-c"}), 0);
  fs::current_path(saved);
  const std::string c = slurp((proj / "build" / "main.c").string());
  EXPECT_NE(c.find("#include \"main.h\""), std::string::npos);
  EXPECT_NE(c.find("_fn_add(void *env"), std::string::npos);
  EXPECT_NE(c.find("extern "), std::string::npos);
  EXPECT_TRUE(fs::exists(proj / "build" / "math.c"));
  fs::remove_all(proj, ec);
}

/* 依赖链/菱形：a → b → c，且 a 同时直接 import c（c 只编译一次）。 */
TEST(CBackend, BuildImportChainAndDiamond) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "imp_chain";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("c.cx", "export func base(x: i32): i32 { return x + 1; }\n");
  write("b.cx",
        "import c from \"./c\";\n"
        "export func mid(x: i32): i32 { return c::base(x) * 2; }\n");
  write("a.cx",
        "import b from \"./b\";\n"
        "import c from \"./c\";\n"
        "func main(): i32 { return b::mid(c::base(20)) - 44; }\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(run_cli({"build", "a.cx"}), 0);
  fs::current_path(saved);
#ifdef _WIN32
  const char *exe_name = "a.exe";
  const char *obj_ext = ".obj";
#else
  const char *exe_name = "a";
  const char *obj_ext = ".o";
#endif
  EXPECT_TRUE(fs::exists(proj / "build" / exe_name));
  EXPECT_TRUE(fs::exists(proj / "build" / ("c" + std::string(obj_ext))));
  EXPECT_EQ(run_exe((proj / "build" / exe_name).string()), 0);
  fs::remove_all(proj, ec);
}

/* 循环 import 必须在编译期报错（driver 的路径栈检测）。 */
TEST(CBackend, RejectsCircularImport) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "imp_cycle";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("a.cx",
        "import b from \"./b\";\n"
        "export func fa(): i32 { return b::fb(); }\n"
        "func main(): i32 { return fa(); }\n");
  write("b.cx",
        "import a from \"./a\";\n"
        "export func fb(): i32 { return a::fa(); }\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  const int rc = run_cli({"build", "a.cx"});
  fs::current_path(saved);
  EXPECT_EQ(rc, 1);
  fs::remove_all(proj, ec);
}

/* 代理导出：`export * from "./base"` 的名字属于 base 模块，跨模块调用必须
 * 解析到**定义模块**的符号（前缀用定义模块的 tag，而不是 reexport 方）。 */
TEST(CBackend, BuildImportReexport) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "imp_reexport";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("base.cx", "export func add(a: i32, b: i32): i32 { return a + b; }\n");
  write("mid.cx", "export * from \"./base\";\n");
  write("main.cx",
        "import mid from \"./mid\";\n"
        "func main(): i32 { return mid::add(20, 22) - 42; }\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(run_cli({"build", "main.cx"}), 0);
  fs::current_path(saved);
#ifdef _WIN32
  const char *exe_name = "main.exe";
#else
  const char *exe_name = "main";
#endif
  ASSERT_TRUE(fs::exists(proj / "build" / exe_name));
  EXPECT_EQ(run_exe((proj / "build" / exe_name).string()), 0);
  fs::remove_all(proj, ec);
}

/* 跨模块**泛型实例**调用：实例体在定义模块的 prog 里（sema 追加），
 * 导入方按 `clux_<ownerTag>_fn_<instance>` 生成 extern 并调用；
 * 泛型实例函数在所有模块里都是 non-static（mangling 名唯一）。 */
TEST(CBackend, BuildImportCrossModuleGeneric) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "imp_generic";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("lib.cx",
        "export func id[T](x: T): T { return x; }\n"
        "export struct Box[T] { v: T; }\n");
  write("main.cx",
        "import m from \"./lib\";\n"
        "func main(): i32 {\n"
        "  var b: m::Box[i32] = .m::Box[i32]{ .v = 41 };\n"
        "  return m::id(b.v) + 1 - 42;\n"
        "}\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(run_cli({"build", "main.cx"}), 0);
  fs::current_path(saved);
#ifdef _WIN32
  const char *exe_name = "main.exe";
#else
  const char *exe_name = "main";
#endif
  ASSERT_TRUE(fs::exists(proj / "build" / exe_name));
  EXPECT_EQ(run_exe((proj / "build" / exe_name).string()), 0);
  /* 导入方 .c 里是定义模块的实例符号（`_fn_id$i32`）。 */
  fs::current_path(proj, ec);
  ASSERT_EQ(run_cli({"build", "main.cx", "--emit-c"}), 0);
  fs::current_path(saved);
  const std::string c = slurp((proj / "build" / "main.c").string());
  EXPECT_NE(c.find("$id$i32"), std::string::npos) << c;
  fs::remove_all(proj, ec);
}

/* 跨模块**方法**调用：`p.sum()` 的 `p` 类型定义在别的模块；sema 通过
 * 定义模块的 member_scope 解析方法符号，C 后端生成定义模块的方法符号
 * extern 调用。 */
TEST(CBackend, BuildImportCrossModuleMethod) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "imp_method";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("lib.cx",
        "export struct Point {\n"
        "  x: i32;\n"
        "  func sum(self: *Point): i32 { return self.x + 1; }\n"
        "}\n");
  write("main.cx",
        "import m from \"./lib\";\n"
        "func main(): i32 {\n"
        "  var p: m::Point = .m::Point{ .x = 20 };\n"
        "  return p.sum() + 21 - 42;\n"
        "}\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  const int rc = run_cli({"build", "main.cx"});
  fs::current_path(saved);
  EXPECT_EQ(rc, 0);
#ifdef _WIN32
  const char *exe_name = "main.exe";
#else
  const char *exe_name = "main";
#endif
  ASSERT_TRUE(fs::exists(proj / "build" / exe_name));
  EXPECT_EQ(run_exe((proj / "build" / exe_name).string()), 0);
  fs::remove_all(proj, ec);
}

/* 跨模块泛型函数返回泛型 struct 实例：实例类型经 VM 级按名去重后，
 * 定义模块实例体里的构造与导入方的字段访问看到同一个类型。 */
TEST(CBackend, BuildImportGenericReturnType) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "imp_gen_ret";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("lib.cx",
        "export struct Box[T] { v: T; }\n"
        "export func wrap[T](x: T): Box[T] { return .Box[T]{ .v = x }; }\n");
  write("main.cx",
        "import m from \"./lib\";\n"
        "func main(): i32 {\n"
        "  var b: m::Box[i32] = m::wrap(41);\n"
        "  return b.v + 1 - 42;\n"
        "}\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  const int rc = run_cli({"build", "main.cx"});
  fs::current_path(saved);
  EXPECT_EQ(rc, 0);
#ifdef _WIN32
  const char *exe_name = "main.exe";
#else
  const char *exe_name = "main";
#endif
  ASSERT_TRUE(fs::exists(proj / "build" / exe_name));
  EXPECT_EQ(run_exe((proj / "build" / exe_name).string()), 0);
  fs::remove_all(proj, ec);
}

/* `clux test --backend=c` 与 build 同路径：import 的依赖模块也收集 test 块，
 * 入口模块生成跨模块 runner（test 块 → `__clux_test_N`，不经 main 进入）。 */
TEST(CBackend, TestRunnerAcrossModules) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "test_modules";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("lib.cx",
        "export func add(a: i32, b: i32): i32 { return a + b; }\n"
        "test \"lib.add\" { assert(add(1, 2) == 3); }\n"
        "test \"lib.more\" { assert(add(2, 2) == 4); }\n");
  write("main.cx",
        "import m from \"./lib\";\n"
        "test \"main.one\" { assert(m::add(20, 22) == 42); }\n"
        "func main(): i32 { return 0; }\n");
  write("main_fail.cx",
        "import m from \"./lib\";\n"
        "test \"main.ok\" { assert(m::add(20, 22) == 42); }\n"
        "test \"main.fail\" { assert(m::add(1, 1) == 3); }\n"
        "func main(): i32 { return 0; }\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(run_cli({"test", "main.cx", "--backend=c"}), 0);
  EXPECT_EQ(run_cli({"test", "main_fail.cx", "--backend=c"}), 1);
  fs::current_path(saved);
  fs::remove_all(proj, ec);
}

/* 硬崩溃（abort/panic）不能 longjmp：runner 崩溃后 CLI 回退到逐用例隔离，
 * 崩溃只影响当前用例，后续用例仍然执行。 */
TEST(CBackend, TestRunnerCrashFallback) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "test_crash";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  std::FILE *fp = std::fopen((proj / "crash.cx").string().c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  std::fputs("test \"c.ok\" { assert(1 == 1); }\n"
             "test \"c.crash\" { var o: ?i32 = nil; var v = o.!; "
             "assert(v == 0); }\n"
             "test \"c.after\" { assert(2 == 2); }\n",
             fp);
  std::fclose(fp);
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  const int rc = run_cli({"test", "crash.cx", "--backend=c"});
  fs::current_path(saved);
  EXPECT_EQ(rc, 1);
  fs::remove_all(proj, ec);
}

/* 下标越界保护：数组长度是类型常量，生成的 C 在 `a[i]` 前插入
 * `clux_index_check(i, N)`，越界打印与 VM 相同的 panic 信息并终止（不是
 * 静默读出垃圾值）。 */
TEST(CBackend, IndexBoundsPanic) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var a: [3]i32 = .[3]i32{ 1, 2, 3 };\n"
      "  var i: i32 = 3;\n"
      "  return a[i];\n"
      "}\n",
      "cbackend_index_bounds");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  ASSERT_EQ(run_build(src, exe.string(), nullptr), 0);
  EXPECT_NE(run_exe(exe.string()), 0)
      << "out-of-bounds array read must panic, not silently return";
}

/* `--emit-c` 与 `--emit-obj` 互斥。 */
TEST(CBackend, RejectsConflictingEmitModes) {
  const std::string src =
      write_temp_file("func main(): i32 { return 0; }\n", "cbackend_conflict");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--emit-c", "--emit-obj"}),
            1);
}

TEST(CBackend, RejectsInvalidMainParameters) {
  const std::string src = write_temp_file(
      "func main(x: i32, y: i32): i32 { return x; }\n", "cbackend_param");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe(src);
  exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 1);
}

/* comptime 折叠出的函数值（匿名字面量 / 局部函数）：函数体 AST 由 sema
   登记，C 后端按需物化成 static 函数并构造函数对象。 */
TEST(CBackend, BuildComptimeFunctionValues) {
  const std::string src = write_temp_file(
      "comptime func make_inc(): func(i32) -> i32 {\n"
      "  return func(x: i32): i32 { return x + 1; };\n"
      "}\n"
      "comptime func make_twice(): func(i32) -> i32 {\n"
      "  func t(x: i32): i32 { return x * 2; }\n"
      "  return t;\n"
      "}\n"
      "comptime func double_(a: i32): i32 { return a * 2; }\n"
      "comptime func quad(a: i32): i32 { return double_(double_(a)); }\n"
      "func main(): i32 {\n"
      "  var inc = make_inc();\n"
      "  if (inc(41) != 42) { return 1; }\n"
      "  var t = make_twice();\n"
      "  if (t(21) != 42) { return 2; }\n"
      "  if (quad(3) != 12) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_comptime_fn_values");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* 闭包类型（捕获槽位进入类型身份）+ 泛型可调用对象。 */
TEST(CBackend, BuildClosureTypeAnnotationAndGenericCall) {
  const std::string src = write_temp_file(
      "type F = func |a: i32| (i32) -> i32;\n"
      "func apply[G](f: G, x: i32): i32 { return f(x); }\n"
      "func main(): i32 {\n"
      "  var a: i32 = 3;\n"
      "  var f: F = func |a| (x: i32): i32 { return x + a; };\n"
      "  if (f(4) != 7) { return 1; }\n"
      "  if (apply[F](f, 4) != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_closure_type");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* 不擦除：带捕获闭包不能当作普通 func(...) 使用。 */
TEST(CBackend, RejectsClosureErasureToPlainFunc) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var a: i32 = 3;\n"
      "  var g: func(i32) -> i32 = func |a| (x: i32): i32 { return x + a; };\n"
      "  return g(4);\n"
      "}\n",
      "cbackend_closure_erase");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_build(src, exe.string(), nullptr), 1);
  EXPECT_FALSE(std::filesystem::exists(exe));
}

/* 闭包 env 内联 + 捕获：值里没有 env 堆块，FREE 按字段释放。 */
TEST(CBackend, BuildClosureCaptureInlineEnv) {
  const std::string src = write_temp_file(
      "type F = func |p: *i32| (i32) -> i32;\n"
      "func main(): i32 {\n"
      "  var v: i32 = 5;\n"
      "  var p: *i32 = v.&;\n"
      "  var g: F = (func |(p = (p))| (x: i32): i32 { return p.* + x; });\n"
      "  if (g(4) != 9) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_closure_inline_env");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* 嵌套闭包捕获：外层闭包 捕获一个 闭包；(outer) 必须递归
   深拷贝内层 env（生成两层 `<sig>__cl`），两者独立调用、各释放一次。 */
TEST(CBackend, BuildNestedClosureCaptureClone) {
  const std::string src = write_temp_file(
      "type Inner = func |p: *i32| () -> i32;\n"
      "type Outer = func |f: Inner| () -> i32;\n"
      "func main(): i32 {\n"
      "  var v: i32 = 7;\n"
      "  var p: *i32 = v.&;\n"
      "  var inner: Inner = (func |(p = (p))| (): i32 { return p.*; });\n"
      "  var outer: Outer = (func |(f: Inner = (inner))| (): i32 {\n"
      "      return f(); });\n"
      "  var copy: Outer = (outer);\n"
      "  if (outer() != 7) { return 1; }\n"
      "  if (copy() != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_nested_closure_clone");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* 导出的 C ABI 函数带**闭包类型**参数/返回：跨模块 import 时签名 struct 与
   内联 env struct 必须都进头文件（含捕获槽位类型），调用点按 extern 原型
   传值。覆盖"导出闭包约定"的第一种形态。 */
TEST(CBackend, BuildExportClosureABI) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "exp_closure_abi";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("a.cx",
        "export func mk(base: i32): func |b: i32| (i32) -> i32 {\n"
        "  return func |base| add(x: i32): i32 { return base + x; };\n"
        "}\n"
        "export func call(f: func |b: i32| (i32) -> i32, x: i32): i32 {\n"
        "  return f(x);\n"
        "}\n");
  write("main.cx",
        "import a from \"./a\";\n"
        "func main(): i32 {\n"
        "  var f: func |b: i32| (i32) -> i32 = a::mk(3);\n"
        "  if (a::call(f, 4) != 7) { return 1; }\n"
        "  return 0;\n"
        "}\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  const int build_rc = run_cli({"build", "main.cx"});
  fs::current_path(saved);
  EXPECT_EQ(build_rc, 0);
#ifdef _WIN32
  const char *exe_name = "main.exe";
#else
  const char *exe_name = "main";
#endif
  ASSERT_TRUE(fs::exists(proj / "build" / exe_name));
  EXPECT_EQ(run_exe((proj / "build" / exe_name).string()), 0);
  fs::remove_all(proj, ec);
}

/* 跨模块导出闭包的更多形态：泛型捕获、模块内 struct 捕获、`fatal` 返回的
   闭包（所有权跨模块转移，调用方作用域释放一次）。 */
TEST(CBackend, BuildExportClosureAcrossModules) {
  namespace fs = std::filesystem;
  fs::path proj = fs::path(clux_test_temp_dir()) / "exp_closure_abi2";
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  auto write = [&](const char *name, const char *text) {
    std::FILE *fp = std::fopen((proj / name).string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs(text, fp);
    std::fclose(fp);
  };
  write("a.cx",
        "export struct Box { v: i32; }\n"
        "var g_own: i32 = 7;\n"
        "export func mk[T](v: T): func |v: T| () -> T {\n"
        "  return func |v| (): T { return v; };\n"
        "}\n"
        "export func make_box(v: i32): func |b: Box| () -> i32 {\n"
        "  var b = .Box{ .v = v };\n"
        "  return func |(b: Box = b)| (): i32 { return b.v; };\n"
        "}\n"
        "export func make_own(): func |p: *i32| () -> i32 {\n"
        "  var p: *i32 = g_own.&;\n"
        "  return (func |(p = (p))| (): i32 { return p.*; });\n"
        "}\n");
  write("main.cx",
        "import ma from \"./a\";\n"
        "func main(): i32 {\n"
        "  var f: func |v: i32| () -> i32 = ma::mk[i32](42);\n"
        "  if (f() != 42) { return 1; }\n"
        "  var g: func |b: ma::Box| () -> i32 = ma::make_box(9);\n"
        "  if (g() != 9) { return 2; }\n"
        "  var h: func |p: *i32| () -> i32 = ma::make_own();\n"
        "  if (h() != 7) { return 3; }\n"
        "  return 0;\n"
        "}\n");
  fs::path saved = fs::current_path();
  fs::current_path(proj, ec);
  ASSERT_FALSE(ec);
  const int build_rc = run_cli({"build", "main.cx"});
  fs::current_path(saved);
  EXPECT_EQ(build_rc, 0);
#ifdef _WIN32
  const char *exe_name = "main.exe";
#else
  const char *exe_name = "main";
#endif
  ASSERT_TRUE(fs::exists(proj / "build" / exe_name));
  EXPECT_EQ(run_exe((proj / "build" / exe_name).string()), 0);
  fs::remove_all(proj, ec);
}

/* 捕获"含 槽位的聚合"（Box 内 *i32）：闭包销毁必须按类型递归
   （`__destroy_Box(*Box)`），不能把结构体值当指针 clux_free。 */
TEST(CBackend, BuildClosureAggregateCaptureFree) {
  const std::string src = write_temp_file(
      "struct Box { p: *i32; }\n"
      "type F = func |b: Box| () -> i32;\n"
      "func main(): i32 {\n"
      "  var v: i32 = 7;\n"
      "  var b = .Box{ .p = v.& };\n"
      "  var f: F = (func |(bx: Box = (b))| (): i32 { return bx.p.*; });\n"
      "  if (f() != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_closure_aggregate_capture");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}

/* 闭包放进含 的聚合字段并被深拷贝：(Holder) 触发闭包 env 的
   静态深拷贝（`<sig>__cl`），源与副本的 捕获必须各自独立、各释放一次。 */
TEST(CBackend, BuildClosureInStructClone) {
  const std::string src = write_temp_file(
      "type F = func |p: *i32| (i32) -> i32;\n"
      "struct Holder { cb: F; n: i32; }\n"
      "func main(): i32 {\n"
      "  var v: i32 = 5;\n"
      "  var p: *i32 = v.&;\n"
      "  var h: Holder = .Holder{ .cb = (func |(p = (p))| (x: i32): i32 {\n"
      "      return p.* + x; }), .n = 1 };\n"
      "  var h2: Holder = (h);\n"
      "  if (h.cb(1) != 6) { return 1; }\n"
      "  if (h2.cb(2) != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cbackend_closure_struct_clone");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 0) << err;
  ASSERT_TRUE(std::filesystem::exists(exe));
  EXPECT_EQ(run_exe(exe.string()), 0);
}
