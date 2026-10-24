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

TEST(CBackend, BuildSliceMakeAndSlice) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 1, 2, 3);\n"
      "  if (len(s) != 3usize) { return 1; }\n"
      "  if (s[1] != 2) { return 2; }\n"
      "  var t: ref []i32 = s[1:3];\n"
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

TEST(CBackend, BuildOwnSlice) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []own *i32 = make(own *i32, 2, new i32{5}, new i32{6});\n"
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
      "  var t: ref []const u8 = a[1:3];\n"
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
      "union U { p: own *i32; n: i32; }\n"
      "func main(): i32 {\n"
      "  var r = .R{ .v = .?i32{7} };\n"
      "  if (r.v == nil) { return 1; }\n"
      "  if (r.v.! != 7) { return 2; }\n"
      "  r.v = nil;\n"
      "  if (r.v != nil) { return 3; }\n"
      "  var u = .U{ .p = new i32{5} };\n"
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
      "  var cap: func(i32) -> i32 = func |a| (x: i32): i32 { return x + a; };\n"
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
      "  func get(self: ref *Point): i32 { return self.x; }\n"
      "  func plus(self: ref *Point, y: i32): i32 { return self.x + y; }\n"
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

TEST(CBackend, BuildOwnCaptureClosure) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{5};\n"
      "  var f: own func(i32) -> i32 =\n"
      "      move(func |(p = move(p))| (x: i32): i32 { return p.* + x; });\n"
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

TEST(CBackend, BuildCloneCaptureClosure) {
  const std::string src = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{5};\n"
      "  var f: own func(i32) -> i32 =\n"
      "      move(func |(p = move(p))| (x: i32): i32 { return p.* + x; });\n"
      "  var g: own func(i32) -> i32 = clone(f);\n"
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
  std::filesystem::path exe = default_exe(src);
  std::error_code ec;
  std::filesystem::remove(c_path, ec);
  std::filesystem::remove(exe, ec);
  EXPECT_EQ(run_cli({"build", std::filesystem::absolute(src).string(),
                     "--emit-c"}),
            0);
  ASSERT_TRUE(std::filesystem::exists(c_path)) << c_path.string();
  const std::string text = slurp(c_path.string());
  EXPECT_NE(text.find("generated by clux C backend"), std::string::npos);
  EXPECT_NE(text.find("clux_fn_main"), std::string::npos);
  /* --emit-c 只生成 C，不调用宿主编译器。 */
  EXPECT_FALSE(std::filesystem::exists(exe));
  std::filesystem::remove(c_path);
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
  EXPECT_NE(text.find("int main(void)"), std::string::npos);
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
      "  var p: ref *B = x.&;\n"
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
      "  var p: ref *S = s.&;\n"
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
      "  func run(self: ref *User): i32 { return 1; }\n"
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
      "  func ok(self: ref *const Result[T, E]): bool { return self.good; }\n"
      "  func value(self: ref *const Result[T, E]): T { return self.val.!; }\n"
      "  func error(self: ref *const Result[T, E]): E { return self.err.!; }\n"
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

TEST(CBackend, RejectsMainWithParameters) {
  const std::string src = write_temp_file(
      "func main(x: i32): i32 { return x; }\n", "cbackend_param");
  ASSERT_FALSE(src.empty());
  std::filesystem::path exe(src);
  exe = default_exe(src);
  std::string err;
  EXPECT_EQ(run_build(src, exe.string(), &err), 1);
}
